#include "../db/db_manager.h"
#include "http_client.h"
#include "message_delivery.h"
#include "retry_worker.h"
#include "crypto/key_manager.h"
#include "crypto/message_crypto.h"
#include <crow.h>
#include <iostream>
#include <ctime>
#include <thread>
#include <atomic>
#include <optional>

constexpr int DEFAULT_PORT = 18080;
constexpr const char* DEFAULT_DB_PATH = "NoMiddle.db";

int main(int argc, char* argv[]) {
    auto keys = load_or_create_keypair("private_key.bin");
    if (!keys) {
        std::cerr << "Failed to load or generate keypair\n";
        return 3;
    }
    std::string self_public_key = keys->public_key_b64;
    std::cout << "My public key: " << self_public_key << "\n";

    
    int port = DEFAULT_PORT;
    std::string db_path = DEFAULT_DB_PATH;
    if (argc > 1) {
        try {
            port = std::stoi(argv[1]);
            if (port <= 0 || port > 65535) {
                throw std::out_of_range("port out of range");
            }
        } catch (const std::exception &) {
            std::cerr << "Invalid port argument: " << argv[1] << "\n";
            return 1;
        }
    }
    if (argc > 2) {
        db_path = argv[2];
    }

    std::unique_ptr<db_manager> db_ptr;
    try {
        db_ptr = std::make_unique<db_manager>(db_path);
    }
    catch (const std::exception &e) {
        std::cerr << "[SQL] Failed to initialize database: " << e.what() << '\n';
        return 1;
    }
    db_manager &db = *db_ptr;

    std::atomic<bool> retry_worker_running{true};
    std::thread thread_retry_worker(start_retrying_worker, std::ref(db), std::ref(retry_worker_running));

    crow::SimpleApp app;

    CROW_ROUTE(app, "/")
    ([](){
        return crow::response(200, "ok");
    });

    CROW_ROUTE(app, "/api/add_contact").methods(crow::HTTPMethod::POST)
    ([&db](const crow::request &req) {
        auto data_json = crow::json::load(req.body);
        if (!data_json) {
            return crow::response(400, crow::json::wvalue{{"error", "Invalid JSON"}});
        }
        if (!data_json.has("name") || !data_json.has("server_address") || !data_json.has("contact_id")) {
            return crow::response(400, crow::json::wvalue{{"error",
                "Missing contact_id, name or server address argument"}});
        }
        std::string contact_id = data_json["contact_id"].s();
        std::string name = data_json["name"].s();
        std::string server_address = data_json["server_address"].s();
        if (!db.add_contact(contact_id, name, server_address)) {
            return crow::response(500, crow::json::wvalue{{"error", "Failed to add contact"}});
        }
        return crow::response(200, "Contact added.");
    });

    CROW_ROUTE(app, "/api/send_message").methods(crow::HTTPMethod::POST)
    ([&db, &self_public_key, &keys](const crow::request &req) {
        auto data_json = crow::json::load(req.body);
        if (!data_json) {
            return crow::response(400, crow::json::wvalue{{"error", "Invalid JSON"}});
        }
        if (!data_json.has("recipient_id") || !data_json.has("text")) {
            return crow::response(400, crow::json::wvalue{{"error", "Missing recipient_id or text"}});
        }
        std::string recipient_id = data_json["recipient_id"].s();
        std::string plaintext = data_json["text"].s();
        int64_t timestamp = static_cast<int64_t>(std::time(nullptr));
        std::optional<std::string> ciphertext = encrypt_message(plaintext, recipient_id, keys->private_key_b64);
        if (!ciphertext) {
            return crow::response(400, crow::json::wvalue{{"error", "Failed to encrypt message"}});
        }
        auto message_id = deliver_message(db, self_public_key, recipient_id, *ciphertext, timestamp);
        if(!message_id.has_value()) {
            return crow::response(400, crow::json::wvalue{{"error", "Error while delivering message"}});
        }
        crow::json::wvalue res;
        res["message_id"] = message_id.value();
        return crow::response(200, res);
    });

    CROW_ROUTE(app, "/accept_message").methods(crow::HTTPMethod::POST)
    ([&db, &self_public_key, &keys](const crow::request &req) {
        auto data_json = crow::json::load(req.body);
        if (!data_json) {
            return crow::response(400, crow::json::wvalue{{"error", "Invalid JSON"}});
        }
        std::string sender_id = data_json["sender_id"].s();
        std::string recipient_id = data_json["recipient_id"].s();
        std::string ciphertext = data_json["text"].s();
        int64_t timestamp = data_json["timestamp"].i();
        if (recipient_id != self_public_key) {
            return crow::response(400, crow::json::wvalue{{"error", "Message not intended for this user"}});
        }
        std::optional<std::string> plaintext = decrypt_message(ciphertext, sender_id, keys->private_key_b64);
        if (!plaintext) {
            return crow::response(400, crow::json::wvalue{{"error", "Failed to decrypt message"}});
        }
        db_manager::message msg{
            data_json["message_id"].s(),
            sender_id,
            recipient_id,
            ciphertext,
            true,
            timestamp
        };
        if (!db.add_message(msg)) {
            return crow::response(500, crow::json::wvalue{{"error", "Failed to store message"}});
        }
        return crow::response(200);
    });

    std::cout << "Server listening on http://0.0.0.0:" << port << "\n";
    app.port(port).bindaddr("0.0.0.0").multithreaded().run();

    retry_worker_running = false;
    thread_retry_worker.join();
}