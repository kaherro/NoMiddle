#include "../db/db_manager.h"
#include "http_client.h"
#include "message_delivery.h"
#include "retry_worker.h"
#include <crow.h>
#include <iostream>
#include <ctime>
#include <thread>
#include <atomic>

constexpr int64_t SELF_ID = 0;
constexpr int DEFAULT_PORT = 18080;
constexpr const char* DEFAULT_DB_PATH = "NoMiddle.db";

int main(int argc, char* argv[]) {
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
        if (!data_json.has("name") || !data_json.has("server_address") || !data_json.has("public_key")) {
            return crow::response(400, crow::json::wvalue{{"error",
                "Missing name, server address or public_key argument"}});
        }
        std::string name = data_json["name"].s();
        std::string server_address = data_json["server_address"].s();
        std::string public_key = data_json["public_key"].s();
        if (!db.add_contact(name, server_address, public_key)) {
            return crow::response(500, crow::json::wvalue{{"error", "Failed to add contact"}});
        }
        return crow::response(200, "Contact added.");
    });

    CROW_ROUTE(app, "/api/send_message").methods(crow::HTTPMethod::POST)
    ([&db](const crow::request &req) {
        auto data_json = crow::json::load(req.body);
        if (!data_json) {
            return crow::response(400, crow::json::wvalue{{"error", "Invalid JSON"}});
        }
        if (!data_json.has("recipient_id") || !data_json.has("text")) {
            return crow::response(400, crow::json::wvalue{{"error", "Missing recipient_id or text"}});
        }

        int64_t recipient_id = data_json["recipient_id"].i();
        std::string text = data_json["text"].s();
        int64_t timestamp = static_cast<int64_t>(std::time(nullptr));
        std::string message_id = deliver_message(db, SELF_ID, recipient_id, text, timestamp);
        crow::json::wvalue res;
        res["message_id"] = message_id;
        return crow::response(200, res);
    });

    CROW_ROUTE(app, "/accept_message").methods(crow::HTTPMethod::POST)
    ([&db](const crow::request &req) {
        auto data_json = crow::json::load(req.body);
        if (!data_json) {
            return crow::response(400, crow::json::wvalue{{"error", "Invalid JSON"}});
        }

        db_manager::message msg{
            data_json["message_id"].s(),
            data_json["sender_id"].i(),
            data_json["recipient_id"].i(),
            data_json["text"].s(),
            true,
            data_json["timestamp"].i()
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