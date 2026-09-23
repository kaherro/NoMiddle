#define CROW_ENABLE_SSL
#include "../db/db_manager.h"
#include "http_client.h"
#include "message_delivery.h"
#include "retry_worker.h"
#include "auth.h"
#include "crypto/key_manager.h"
#include "crypto/message_crypto.h"
#include <crow.h>
#include <iostream>
#include <ctime>
#include <thread>
#include <atomic>
#include <optional>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <algorithm>

constexpr int DEFAULT_PORT = 18080;
constexpr const char* DEFAULT_DB_PATH = "NoMiddle.db";
constexpr const char* DEFAULT_CERT_PATH = "cert.pem";
constexpr const char* DEFAULT_KEY_PATH  = "key.pem";

std::string readFile(const std::string& basePath, const std::string& requestedPath) {
    if (requestedPath.find("..") != std::string::npos) {
        return "";
    }
    std::string fullPath = basePath + "/" + requestedPath;
    std::ifstream file(fullPath, std::ios::in | std::ios::binary);
    if (!file) {
        return "";
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

bool has_suffix(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string key_path_for_db(const std::string &db_path, int port) {
    size_t slash = db_path.find_last_of("/\\");
    std::string dir = (slash == std::string::npos) ? "" : db_path.substr(0, slash);
    return dir + (dir.empty() ? "" : "/") + "private_key" + std::to_string(port) + ".bin";
}

std::string content_type_for(const std::string &path) {
    if (has_suffix(path, ".html")) return "text/html";
    if (has_suffix(path, ".css"))  return "text/css";
    if (has_suffix(path, ".js"))   return "text/javascript";
    if (has_suffix(path, ".png"))  return "image/png";
    if (has_suffix(path, ".svg"))  return "image/svg+xml";
    if (has_suffix(path, ".ico"))  return "image/x-icon";
    return "application/octet-stream";
}

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

    std::string cert_path = DEFAULT_CERT_PATH;
    std::string key_path  = DEFAULT_KEY_PATH;
    if (argc > 3) cert_path = argv[3];
    if (argc > 4) key_path  = argv[4];

    auto keys = load_or_create_keypair(key_path_for_db(db_path, port));
    if (!keys) {
        std::cerr << "Failed to load or generate keypair\n";
        return 3;
    }
    std::string self_public_key = keys->public_key_b64;
    std::cout << "My public key: " << self_public_key << "\n";

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

    std::mutex ws_mutex;
    std::unordered_set<crow::websocket::connection*> ws_clients;
    std::unordered_map<std::string, std::vector<crow::websocket::connection*>> ws_client_ids;

    auto notify_auth_request = [&ws_mutex, &ws_clients](const std::string &request_id, const std::string &device_name, int64_t created_at) {
        crow::json::wvalue payload;
        payload["type"]        = "auth_request";
        payload["request_id"]  = request_id;
        payload["device_name"] = device_name;
        payload["created_at"]  = created_at;
        std::string data = payload.dump();

        std::lock_guard<std::mutex> lock(ws_mutex);
        for (auto* conn : ws_clients) {
            conn->send_text(data);
        }
    };

    auto notify_new_message = [&ws_mutex, &ws_clients](const std::string &message_id, const std::string &other_party_id) {
        crow::json::wvalue payload;
        payload["type"]       = "new_message";
        payload["message_id"] = message_id;
        payload["contact_id"] = other_party_id; 
        std::string data = payload.dump();

        std::lock_guard<std::mutex> lock(ws_mutex);
        for (auto* conn : ws_clients) {
            conn->send_text(data);
        }
    };

    auto notify_edit_message = [&ws_mutex, &ws_clients](const std::string &message_id, const std::string &other_party_id) {
        crow::json::wvalue payload;
        payload["type"]       = "edit_message";
        payload["message_id"] = message_id;
        payload["contact_id"] = other_party_id; 
        std::string data = payload.dump();

        std::lock_guard<std::mutex> lock(ws_mutex);
        for (auto* conn : ws_clients) {
            conn->send_text(data);
        }
    };

    auto notify_delete_message = [&ws_mutex, &ws_clients](const std::string &message_id, const std::string &other_party_id) {
        crow::json::wvalue payload;
        payload["type"]       = "delete_message";
        payload["message_id"] = message_id;
        payload["contact_id"] = other_party_id; 
        std::string data = payload.dump();

        std::lock_guard<std::mutex> lock(ws_mutex);
        for (auto* conn : ws_clients) {
            conn->send_text(data);
        }
    };

    auto is_authed = [&db](const crow::request &req) {
        return authenticate(db, header_value(req, "X-Client-Id"), header_value(req, "X-Client-Secret"));
    };

    crow::SimpleApp app;

    CROW_ROUTE(app, "/")
    ([](){
        return crow::response(200, "ok");
    });

    CROW_ROUTE(app, "/<string>")
    ([](const crow::request& req, std::string path){
        if (path.empty()) path = "index.html";
        std::string content = readFile("web", path);
        if (!content.empty()) {
            crow::response res(content);
            res.set_header("Content-Type", content_type_for(path));
            return res;
        }
        return crow::response(404);
    });

    CROW_ROUTE(app, "/ws/messages")
    .websocket(&app)
    .onopen([](crow::websocket::connection &conn) {
        std::cout << "[WS] Connection opened, waiting for auth\n";
    })
    .onclose([&ws_mutex, &ws_clients, &ws_client_ids](crow::websocket::connection &conn, 
        const std::string &reason, unsigned short close_code) {
        std::lock_guard<std::mutex> lock(ws_mutex);
        ws_clients.erase(&conn);
        for (auto &entry : ws_client_ids) {
            auto &vec = entry.second;
            vec.erase(std::remove(vec.begin(), vec.end(), &conn), vec.end());
        }
        std::cout << "[WS] Client disconnected: " << reason << "\n";
    })
    .onmessage([&db, &ws_mutex, &ws_clients, &ws_client_ids](crow::websocket::connection &conn, 
        const std::string &data, bool is_binary) {
        if (is_binary) return;
        auto parsed = crow::json::load(data);
        if (!parsed || !parsed.has("type") || parsed["type"].t() != crow::json::type::String) return;
        std::string type = parsed["type"].s();
        if (type != "auth") return;
        if (!parsed.has("client_id") || !parsed.has("secret") ||
            parsed["client_id"].t() != crow::json::type::String ||
            parsed["secret"].t() != crow::json::type::String) {
            conn.close("unauthorized");
            return;
        }
        if (!authenticate(db, parsed["client_id"].s(), parsed["secret"].s())) {
            conn.close("unauthorized");
            return;
        }
        std::lock_guard<std::mutex> lock(ws_mutex);
        ws_clients.insert(&conn);
        if (parsed.has("client_id") && parsed["client_id"].t() == crow::json::type::String) {
            ws_client_ids[parsed["client_id"].s()].push_back(&conn);
        }
        conn.send_text("{\"type\":\"auth_ok\"}");
        std::cout << "[WS] Client authorized, total: " << ws_clients.size() << "\n";
    });

    CROW_ROUTE(app, "/api/upsert_contact").methods(crow::HTTPMethod::PUT)
    ([&db, &is_authed](const crow::request &req) {
        if (!is_authed(req)) {
            return crow::response(401, crow::json::wvalue{{"error", "Unauthorized"}});
        }
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
        if (!db.upsert_contact(contact_id, name, server_address)) {
            return crow::response(500, crow::json::wvalue{{"error", "Failed to add contact"}});
        }
        return crow::response(200, "Contact added.");
    });

    CROW_ROUTE(app, "/api/send_message").methods(crow::HTTPMethod::POST)
    ([&db, &self_public_key, &keys, &notify_new_message, &is_authed](const crow::request &req) {
        if (!is_authed(req)) {
            return crow::response(401, crow::json::wvalue{{"error", "Unauthorized"}});
        }
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
        auto message_id = deliver_message(db, self_public_key, recipient_id, *ciphertext, plaintext, timestamp);
        if(!message_id.has_value()) {
            return crow::response(400, crow::json::wvalue{{"error", "Error while delivering message"}});
        }
        notify_new_message(message_id.value(), recipient_id);
        crow::json::wvalue res;
        res["message_id"] = message_id.value();
        return crow::response(200, res);
    });

    CROW_ROUTE(app, "/api/edit_message").methods(crow::HTTPMethod::POST)
    ([&db, &self_public_key, &keys, &notify_edit_message, &is_authed](const crow::request &req) {
        if (!is_authed(req)) {
            return crow::response(401, crow::json::wvalue{{"error", "Unauthorized"}});
        }
        auto data_json = crow::json::load(req.body);
        if (!data_json) {
            return crow::response(400, crow::json::wvalue{{"error", "Invalid JSON"}});
        }
        if (!data_json.has("message_id") || !data_json.has("text")) {
            return crow::response(400, crow::json::wvalue{{"error", "Missing message_id or text"}});
        }
        std::string message_id = data_json["message_id"].s();
        std::string plaintext = data_json["text"].s();
        db_manager::message msg;
        if (!db.get_message(message_id, msg)) {
            return crow::response(404, crow::json::wvalue{{"error", "Unknown message"}});
        }
        if (msg.sender_id != self_public_key) {
            return crow::response(403, crow::json::wvalue{{"error", "Cannot edit message sent by someone else"}});
        }
        std::optional<std::string> ciphertext = encrypt_message(plaintext, msg.recipient_id, keys->private_key_b64);
        if (!ciphertext) {
            return crow::response(400, crow::json::wvalue{{"error", "Failed to encrypt message"}});
        }
        int64_t edited_at = static_cast<int64_t>(std::time(nullptr));
        if (!db.update_message_edit(message_id, plaintext, *ciphertext, edited_at)) {
            return crow::response(500, crow::json::wvalue{{"error", "Failed to update message"}});
        }
        deliver_message_edit(db, message_id, self_public_key, msg.recipient_id, *ciphertext, edited_at);
        notify_edit_message(message_id, msg.recipient_id);
        crow::json::wvalue res;
        res["message_id"] = message_id;
        res["plaintext"] = plaintext;
        res["edited_at"] = edited_at;
        return crow::response(200, res);
    });

    CROW_ROUTE(app, "/api/delete_message").methods(crow::HTTPMethod::POST)
    ([&db, &self_public_key, &notify_delete_message, &is_authed](const crow::request &req) {
        if (!is_authed(req)) {
            return crow::response(401, crow::json::wvalue{{"error", "Unauthorized"}});
        }
        auto data_json = crow::json::load(req.body);
        if (!data_json) {
            return crow::response(400, crow::json::wvalue{{"error", "Invalid JSON"}});
        }
        if (!data_json.has("message_id")) {
            return crow::response(400, crow::json::wvalue{{"error", "Missing message_id"}});
        }
        std::string message_id = data_json["message_id"].s();
        db_manager::message msg;
        if (!db.get_message(message_id, msg)) {
            return crow::response(404, crow::json::wvalue{{"error", "Unknown message"}});
        }
        if (msg.sender_id != self_public_key) {
            return crow::response(403, crow::json::wvalue{{"error", "Cannot delete message sent by someone else"}});
        }
        db.mark_deleted(message_id);
        db.mark_delete_pending(message_id);
        deliver_message_delete(db, message_id, self_public_key, msg.recipient_id);
        notify_delete_message(message_id, msg.recipient_id);
        crow::json::wvalue res;
        res["message_id"] = message_id;
        return crow::response(200, res);
    });

    CROW_ROUTE(app, "/accept_message").methods(crow::HTTPMethod::POST)
    ([&db, &self_public_key, &keys, &notify_new_message](const crow::request &req) {
        auto data_json = crow::json::load(req.body);
        if(!data_json) {
            return crow::response(400, crow::json::wvalue{{"error", "Invalid JSON"}});
        }
        if(!data_json.has("sender_id") || !data_json.has("recipient_id") || !data_json.has("ciphertext") || !data_json.has("timestamp")) {
            return crow::response(400, crow::json::wvalue{{"error", "Missing of the arguments"}});
        }
        std::string sender_id = data_json["sender_id"].s();
        std::string recipient_id = data_json["recipient_id"].s();
        std::string ciphertext = data_json["ciphertext"].s();
        int64_t timestamp = data_json["timestamp"].i();
        if (recipient_id != self_public_key) {
            return crow::response(400, crow::json::wvalue{{"error", "Message not intended for this user"}});
        }
        std::optional<std::string> plaintext = decrypt_message(ciphertext, sender_id, keys->private_key_b64);
        if (!plaintext) {
            return crow::response(400, crow::json::wvalue{{"error", "Failed to decrypt message"}});
        }
        std::string message_id = data_json["message_id"].s();
        std::string group_id;
        if (data_json.has("group_id") && data_json["group_id"].t() == crow::json::type::String) {
            group_id = data_json["group_id"].s();
        }
        db_manager::message msg{
            message_id,
            sender_id,
            recipient_id,
            group_id,
            plaintext.value(),
            ciphertext,
            true,
            timestamp,
            0,
            0,
            1,
            0
        };
        if (!db.add_message(msg)) {
            return crow::response(500, crow::json::wvalue{{"error", "Failed to store message"}});
        }
        notify_new_message(message_id, sender_id);
        return crow::response(200);
    });
    
    CROW_ROUTE(app, "/accept_edit").methods(crow::HTTPMethod::POST)
    ([&db, &self_public_key, &keys, &notify_edit_message](const crow::request &req) {
        auto data_json = crow::json::load(req.body);
        if(!data_json) {
            return crow::response(400, crow::json::wvalue{{"error", "Invalid JSON"}});
        }
        if(!data_json.has("message_id") || !data_json.has("sender_id") || !data_json.has("recipient_id") ||
            !data_json.has("ciphertext") || !data_json.has("edited_at")) {
            return crow::response(400, crow::json::wvalue{{"error", "Missing of the arguments"}});
        }
        std::string message_id = data_json["message_id"].s();
        std::string sender_id = data_json["sender_id"].s();
        std::string recipient_id = data_json["recipient_id"].s();
        std::string ciphertext = data_json["ciphertext"].s();
        int64_t edited_at = data_json["edited_at"].i();
        if (recipient_id != self_public_key) {
            return crow::response(400, crow::json::wvalue{{"error", "Message edit not intended for this user"}});
        }
        db_manager::message msg;
        if (!db.get_message(message_id, msg)) {
            return crow::response(404, crow::json::wvalue{{"error", "Unknown message"}});
        }
        std::optional<std::string> plaintext = decrypt_message(ciphertext, sender_id, keys->private_key_b64);
        if (!plaintext) {
            return crow::response(400, crow::json::wvalue{{"error", "Failed to decrypt message"}});
        }
        if (!db.update_message_edit(message_id, plaintext.value(), ciphertext, edited_at)) {
            return crow::response(500, crow::json::wvalue{{"error", "Failed to update message"}});
        }
        db.mark_edit_accepted(message_id);
        notify_edit_message(message_id, sender_id);
        return crow::response(200);
    });
    
    CROW_ROUTE(app, "/accept_delete").methods(crow::HTTPMethod::POST)
    ([&db, &self_public_key, &notify_delete_message](const crow::request &req) {
        auto data_json = crow::json::load(req.body);
        if(!data_json) {
            return crow::response(400, crow::json::wvalue{{"error", "Invalid JSON"}});
        }
        if(!data_json.has("message_id") || !data_json.has("sender_id") || !data_json.has("recipient_id")) {
            return crow::response(400, crow::json::wvalue{{"error", "Missing of the arguments"}});
        }
        std::string message_id = data_json["message_id"].s();
        std::string sender_id = data_json["sender_id"].s();
        std::string recipient_id = data_json["recipient_id"].s();
        if (recipient_id != self_public_key) {
            return crow::response(400, crow::json::wvalue{{"error", "Message delete not intended for this user"}});
        }
        db_manager::message msg;
        if (!db.get_message(message_id, msg)) {
            return crow::response(200);
        }
        if (msg.sender_id != sender_id) {
            return crow::response(400, crow::json::wvalue{{"error", "Sender does not match message"}});
        }
        db.mark_deleted(message_id);
        notify_delete_message(message_id, sender_id);
        return crow::response(200);
    });
    
    CROW_ROUTE(app, "/api/contacts").methods(crow::HTTPMethod::GET)
    ([&db, &self_public_key, &is_authed](const crow::request &req) {
        if (!is_authed(req)) {
            return crow::response(401, crow::json::wvalue{{"error", "Unauthorized"}});
        }
        auto contacts = db.get_contacts_with_latest_message(self_public_key);
        crow::json::wvalue result;
        std::vector<crow::json::wvalue> res;
        for (const auto &c : contacts) {
            crow::json::wvalue obj;
            obj["contact_id"] = c.contact_id;
            obj["name"] = c.name;
            obj["server_address"] = c.server_address;
            if (c.latest_message) {
                const auto &m = *c.latest_message;
                crow::json::wvalue msg;
                msg["message_id"] = m.message_id;
                msg["sender_id"] = m.sender_id;
                msg["recipient_id"] = m.recipient_id;
                msg["plaintext"] = m.plaintext;
                msg["accepted"] = m.accepted;
                msg["timestamp"] = m.timestamp;
                msg["edited_at"] = m.edited_at;
                obj["latest_message"] = std::move(msg);
            } 
            else {
                obj["latest_message"] = nullptr;
            }
            res.push_back(std::move(obj));
        }
        result["contacts"] = std::move(res);
        return crow::response(200, result);
    });

    CROW_ROUTE(app, "/api/messages").methods(crow::HTTPMethod::GET)
    ([&db, &self_public_key, &is_authed](const crow::request &req) {
        if (!is_authed(req)) {
            return crow::response(401, crow::json::wvalue{{"error", "Unauthorized"}});
        }
        const char* contact_cstr = req.url_params.get("contact_id");
        if (!contact_cstr) {
            return crow::response(400, crow::json::wvalue{{"error", "Missing contact_id parameter"}});
        }
        std::string contact_id(contact_cstr);
        auto messages = db.get_messages_between(self_public_key, contact_id);
        crow::json::wvalue result;
        std::vector<crow::json::wvalue> arr;
        for (const auto &m : messages) {
            crow::json::wvalue obj;
            obj["message_id"] = m.message_id;
            obj["sender_id"] = m.sender_id;
            obj["recipient_id"] = m.recipient_id;
            obj["plaintext"] = m.plaintext;
            obj["accepted"] = m.accepted;
            obj["timestamp"] = m.timestamp;
            obj["edited_at"] = m.edited_at;
            arr.push_back(std::move(obj));
        }
        result["messages"] = std::move(arr);
        return crow::response(200, result);
    });

    CROW_ROUTE(app, "/api/groups/create").methods(crow::HTTPMethod::POST)
    ([&db, &self_public_key, &is_authed](const crow::request &req) {
        if (!is_authed(req)) return crow::response(401, crow::json::wvalue{{"error", "Unauthorized"}});
        auto data_json = crow::json::load(req.body);
        if (!data_json || !data_json.has("name")) {
            return crow::response(400, crow::json::wvalue{{"error", "Invalid JSON or missing name"}});
        }
        std::string group_id = generate_uuid();
        db_manager::group g;
        g.group_id = group_id;
        g.name = data_json["name"].s();
        g.created_by = self_public_key;
        g.created_at = static_cast<int64_t>(std::time(nullptr));
        if (!db.create_group(g)) {
            return crow::response(500, crow::json::wvalue{{"error", "Failed to create group"}});
        }
        std::vector<db_manager::group_member> members;
        db_manager::group_member me;
        me.group_id = group_id;
        me.member_id = self_public_key;
        me.server_address = "";
        me.role = "admin";
        me.added_at = static_cast<int64_t>(std::time(nullptr));
        members.push_back(me);
        if (!db.replace_group_members(group_id, members)) {
            return crow::response(500, crow::json::wvalue{{"error", "Failed to register admin member"}});
        }
        crow::json::wvalue result;
        result["group_id"] = group_id;
        return crow::response(200, result);
    });

    CROW_ROUTE(app, "/api/groups/<string>/add_member").methods(crow::HTTPMethod::POST)
    ([&db, &self_public_key, &is_authed](const crow::request &req, const std::string &group_id) {
        if (!is_authed(req)) return crow::response(401, crow::json::wvalue{{"error", "Unauthorized"}});
        auto data_json = crow::json::load(req.body);
        if (!data_json || !data_json.has("member_id") || !data_json.has("server_address")) {
            return crow::response(400, crow::json::wvalue{{"error", "Invalid JSON or missing member info"}});
        }
        std::string member_id = data_json["member_id"].s();
        std::string server_address = data_json["server_address"].s();

        auto existing = db.get_group_members(group_id);
        bool found = false;
        for (auto &m : existing) {
            if (m.member_id == member_id) { m.server_address = server_address; found = true; break; }
        }
        if (!found) {
            db_manager::group_member nm;
            nm.group_id = group_id;
            nm.member_id = member_id;
            nm.server_address = server_address;
            nm.role = "member";
            nm.added_at = static_cast<int64_t>(std::time(nullptr));
            existing.push_back(nm);
        }
        if (!db.replace_group_members(group_id, existing)) {
            return crow::response(500, crow::json::wvalue{{"error", "Failed to update membership"}});
        }
        int64_t version = static_cast<int64_t>(std::time(nullptr));
        bool delivered = deliver_group_update(db, group_id, self_public_key, existing, version);
        crow::json::wvalue result;
        result["delivered"] = delivered;
        return crow::response(200, result);
    });

    CROW_ROUTE(app, "/api/groups/<string>/remove_member").methods(crow::HTTPMethod::POST)
    ([&db, &self_public_key, &is_authed](const crow::request &req, const std::string &group_id) {
        if (!is_authed(req)) return crow::response(401, crow::json::wvalue{{"error", "Unauthorized"}});
        auto data_json = crow::json::load(req.body);
        if (!data_json || !data_json.has("member_id")) {
            return crow::response(400, crow::json::wvalue{{"error", "Invalid JSON or missing member_id"}});
        }
        std::string member_id = data_json["member_id"].s();
        auto existing = db.get_group_members(group_id);
        std::vector<db_manager::group_member> remaining;
        for (const auto &m : existing) {
            if (m.member_id != member_id) remaining.push_back(m);
        }
        if (remaining.size() == existing.size()) {
            return crow::response(404, crow::json::wvalue{{"error", "Member not in group"}});
        }
        if (!db.replace_group_members(group_id, remaining)) {
            return crow::response(500, crow::json::wvalue{{"error", "Failed to update membership"}});
        }
        int64_t version = static_cast<int64_t>(std::time(nullptr));
        bool delivered = deliver_group_update(db, group_id, self_public_key, remaining, version);
        if (delivered && remaining.empty()) {
            db.delete_group(group_id);
        }
        crow::json::wvalue result;
        result["delivered"] = delivered;
        return crow::response(200, result);
    });

    CROW_ROUTE(app, "/api/groups").methods(crow::HTTPMethod::GET)
    ([&db, &is_authed](const crow::request &req) {
        if (!is_authed(req)) return crow::response(401, crow::json::wvalue{{"error", "Unauthorized"}});
        auto groups = db.get_groups();
        crow::json::wvalue result;
        std::vector<crow::json::wvalue> arr;
        for (const auto &g : groups) {
            crow::json::wvalue e;
            e["group_id"] = g.group_id;
            e["name"] = g.name;
            e["created_by"] = g.created_by;
            e["created_at"] = g.created_at;
            arr.push_back(std::move(e));
        }
        result["groups"] = std::move(arr);
        return crow::response(200, result);
    });

    CROW_ROUTE(app, "/api/groups/<string>/members").methods(crow::HTTPMethod::GET)
    ([&db, &is_authed](const crow::request &req, const std::string &group_id) {
        if (!is_authed(req)) return crow::response(401, crow::json::wvalue{{"error", "Unauthorized"}});
        auto members = db.get_group_members(group_id);
        crow::json::wvalue result;
        std::vector<crow::json::wvalue> arr;
        for (const auto &m : members) {
            crow::json::wvalue e;
            e["member_id"] = m.member_id;
            e["server_address"] = m.server_address;
            e["role"] = m.role;
            arr.push_back(std::move(e));
        }
        result["members"] = std::move(arr);
        return crow::response(200, result);
    });

    CROW_ROUTE(app, "/accept_group_update").methods(crow::HTTPMethod::POST)
    ([&db](const crow::request &req) {
        auto data_json = crow::json::load(req.body);
        if (!data_json || !data_json.has("group_id") || !data_json.has("members")) {
            return crow::response(400, crow::json::wvalue{{"error", "Invalid snapshot"}});
        }
        std::string group_id = data_json["group_id"].s();
        std::string sender_id;
        if (data_json.has("sender_id")) sender_id = data_json["sender_id"].s();
        db_manager::group g;
        if (!db.get_group(group_id, g)) {
            db_manager::group ng;
            ng.group_id = group_id;
            ng.name = data_json.has("name") ? data_json["name"].s() : group_id;
            ng.created_by = sender_id;
            ng.created_at = static_cast<int64_t>(std::time(nullptr));
            db.create_group(ng);
        }
        std::vector<db_manager::group_member> members;
        for (const auto &e : data_json["members"]) {
            db_manager::group_member m;
            m.group_id = group_id;
            m.member_id = e["member_id"].s();
            m.server_address = e["server_address"].s();
            m.role = e["role"].s();
            members.push_back(m);
        }
        if (!db.replace_group_members(group_id, members)) {
            return crow::response(500, crow::json::wvalue{{"error", "Failed to store snapshot"}});
        }
        return crow::response(200);
    });

CROW_ROUTE(app, "/api/public_key").methods(crow::HTTPMethod::GET)
    ([&self_public_key](const crow::request &req) {
        crow::json::wvalue result;
        result["public_key"] = self_public_key;
        crow::response res;
        res.add_header("Access-Control-Allow-Origin", "*");
        res.add_header("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
        res.add_header("Access-Control-Allow-Headers", "Content-Type");
        res.code = 200; 
        res.body = result.dump();
        res.set_header("Content-Type", "application/json");
        return res; 
    });

    CROW_ROUTE(app, "/api/auth/register").methods(crow::HTTPMethod::POST)
    ([&db](const crow::request &req) {
        if (db.has_clients()) {
            return crow::response(403, crow::json::wvalue{{"error", "Node is locked"}});
        }
        std::string client_id = random_b64url(16);
        std::string secret = random_b64url(32);
        std::string salt = random_b64url(16);
        std::string secret_hash = hash_secret(secret, salt);
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        if (!db.create_client(client_id, "", salt, secret_hash, now)) {
            return crow::response(500, crow::json::wvalue{{"error", "Failed to register client"}});
        }
        crow::json::wvalue res;
        res["client_id"] = client_id;
        res["secret"] = secret;
        return crow::response(200, res);
    });

    CROW_ROUTE(app, "/api/auth/request").methods(crow::HTTPMethod::POST)
    ([&db, &notify_auth_request](const crow::request &req) {
        std::string device_name = "New device";
        auto data_json = crow::json::load(req.body);
        if (data_json && data_json.has("name") && data_json["name"].t() == crow::json::type::String) {
            std::string n = data_json["name"].s();
            if (!n.empty() && n.size() <= 64) device_name = n;
        }
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        if (!db.has_clients()) {
            std::string client_id = random_b64url(16);
            std::string secret = random_b64url(32);
            std::string salt = random_b64url(16);
            std::string secret_hash = hash_secret(secret, salt);
            if (!db.create_client(client_id, device_name, salt, secret_hash, now)) {
                return crow::response(500, crow::json::wvalue{{"error", "Failed to register client"}});
            }
            crow::json::wvalue res;
            res["auto"] = true;
            res["client_id"] = client_id;
            res["secret"] = secret;
            return crow::response(200, res);
        }
        std::string request_id = random_b64url(16);
        int64_t expires_at = now + 600;
        if (!db.add_auth_request(request_id, device_name, now, expires_at)) {
            return crow::response(500, crow::json::wvalue{{"error", "Failed to create auth request"}});
        }
        notify_auth_request(request_id, device_name, now);
        crow::json::wvalue res;
        res["auto"] = false;
        res["request_id"] = request_id;
        res["expires_at"] = expires_at;
        return crow::response(200, res);
    });

    CROW_ROUTE(app, "/api/auth/approve").methods(crow::HTTPMethod::POST)
    ([&db, &is_authed](const crow::request &req) {
        if (!is_authed(req)) {
            return crow::response(401, crow::json::wvalue{{"error", "Unauthorized"}});
        }
        auto data_json = crow::json::load(req.body);
        if (!data_json || !data_json.has("request_id") || data_json["request_id"].t() != crow::json::type::String ||
            !data_json.has("allow")) {
            return crow::response(400, crow::json::wvalue{{"error", "Missing request_id or allow"}});
        }
        std::string request_id = data_json["request_id"].s();
        db_manager::auth_request req_row;
        if (!db.get_auth_request(request_id, req_row)) {
            return crow::response(404, crow::json::wvalue{{"error", "Unknown auth request"}});
        }
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        if (req_row.expires_at < now) {
            db.delete_auth_request(request_id);
            return crow::response(410, crow::json::wvalue{{"error", "Auth request expired"}});
        }
        if (!req_row.approved_client_id.empty()) {
            return crow::response(409, crow::json::wvalue{{"error", "Already approved"}});
        }
        bool allow = data_json["allow"].t() == crow::json::type::True;
        if (!allow) {
            db.delete_auth_request(request_id);
            return crow::response(200, crow::json::wvalue{{"ok", true}});
        }
        std::string client_id = random_b64url(16);
        std::string secret = random_b64url(32);
        std::string salt = random_b64url(16);
        std::string secret_hash = hash_secret(secret, salt);
        if (!db.create_client(client_id, req_row.device_name, salt, secret_hash, now)) {
            return crow::response(500, crow::json::wvalue{{"error", "Failed to create client"}});
        }
        if (!db.approve_auth_request(request_id, client_id, secret)) {
            db.delete_client(client_id);
            return crow::response(500, crow::json::wvalue{{"error", "Failed to approve request"}});
        }
        return crow::response(200, crow::json::wvalue{{"ok", true}});
    });

    CROW_ROUTE(app, "/api/auth/request").methods(crow::HTTPMethod::GET)
    ([&db](const crow::request &req) {
        const char* rid_param = req.url_params.get("request_id");
        if (!rid_param) {
            return crow::response(400, crow::json::wvalue{{"error", "Missing request_id parameter"}});
        }
        std::string request_id(rid_param);
        db_manager::auth_request req_row;
        if (!db.get_auth_request(request_id, req_row)) {
            return crow::response(200, crow::json::wvalue{{"status", "unknown"}});
        }
        if (req_row.expires_at < static_cast<int64_t>(std::time(nullptr))) {
            db.delete_auth_request(request_id);
            return crow::response(200, crow::json::wvalue{{"status", "expired"}});
        }
        if (req_row.approved_client_id.empty()) {
            crow::json::wvalue res;
            res["status"] = "pending";
            res["device_name"] = req_row.device_name;
            return crow::response(200, res);
        }
        crow::json::wvalue res;
        res["status"] = "approved";
        res["client_id"] = req_row.approved_client_id;
        res["secret"] = req_row.approved_secret;
        db.delete_auth_request(request_id);
        return crow::response(200, res);
    });

    CROW_ROUTE(app, "/api/clients").methods(crow::HTTPMethod::GET)
    ([&db, &is_authed](const crow::request &req) {
        if (!is_authed(req)) {
            return crow::response(401, crow::json::wvalue{{"error", "Unauthorized"}});
        }
        auto clients = db.clients_list();
        crow::json::wvalue result;
        std::vector<crow::json::wvalue> arr;
        for (const auto &c : clients) {
            crow::json::wvalue obj;
            obj["client_id"] = c.client_id;
            obj["device_name"] = c.device_name;
            obj["created_at"] = c.created_at;
            arr.push_back(std::move(obj));
        }
        result["clients"] = std::move(arr);
        return crow::response(200, result);
    });

    CROW_ROUTE(app, "/api/clients/<string>").methods(crow::HTTPMethod::DELETE)
    ([&db, &is_authed, &ws_mutex, &ws_clients, &ws_client_ids](const crow::request &req, std::string client_id) {
        if (!is_authed(req)) {
            return crow::response(401, crow::json::wvalue{{"error", "Unauthorized"}});
        }
        if (header_value(req, "X-Client-Id") == client_id) {
            return crow::response(400, crow::json::wvalue{{"error", "Cannot remove the current device"}});
        }
        if (!db.delete_client(client_id)) {
            return crow::response(404, crow::json::wvalue{{"error", "Unknown client"}});
        }
        std::vector<crow::websocket::connection*> to_close;
        {
            std::lock_guard<std::mutex> lock(ws_mutex);
            auto it = ws_client_ids.find(client_id);
            if (it != ws_client_ids.end()) {
                for (auto* conn : it->second) {
                    ws_clients.erase(conn);
                    to_close.push_back(conn);
                }
                ws_client_ids.erase(it);
            }
        }
        for (auto* conn : to_close) {
            conn->close("revoked");
        }
        return crow::response(200, crow::json::wvalue{{"ok", true}});
    });

    if (cert_path.empty() || cert_path == "none") {
        std::cout << "Server listening on http://0.0.0.0:" << port << "\n";
        app.port(port).bindaddr("0.0.0.0").multithreaded().run();
    } 
    else {
        std::cout << "Server listening on https://0.0.0.0:" << port << " (TLS: " << cert_path << " / " << key_path << ")\n";
        app.port(port).bindaddr("0.0.0.0").multithreaded().ssl_file(cert_path, key_path).run();
    }

    retry_worker_running = false;
    thread_retry_worker.join();
}