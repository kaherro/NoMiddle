#include "../db/db_manager.h"
#include "http_client.h"
#include "message_delivery.h"
#include <crow.h>
#include <iostream>
#include <ctime>

constexpr int64_t SELF_ID = 0;

int main() {
    std::unique_ptr<db_manager> db_ptr;
    try {
        db_ptr = std::make_unique<db_manager>("NoMiddle.db");
    }
    catch (const std::exception &e) {
        std::cerr << "[SQL] Failed to initialize database: " << e.what() << '\n';
        return 1;
    }
    db_manager &db = *db_ptr;

    crow::SimpleApp app;

    CROW_ROUTE(app, "/")
    ([](){
        return crow::response(200, "ok");
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

    std::cout << "Server listening on http://0.0.0.0:18080\n";
    app.port(18080).bindaddr("0.0.0.0").multithreaded().run();
}