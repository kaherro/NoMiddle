#include "message_delivery.h"
#include "http_client.h"
#include <crow.h>
#include <random>
#include <sstream>
#include <iomanip>

std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist(0, 15);
    const char* chars = "0123456789abcdef"; 
    std::string uuid; 
    for(int i = 0; i < 36; i++) {
        if(i == 8 || i == 13 || i == 18 || i == 23) {
            uuid.push_back('-'); 
        }
        else if(i == 14) {
            uuid.push_back('4'); 
        }
        else if(i == 19) {
            uuid.push_back(chars[dist(gen) & 3 | 8]);
        }
        else {
            uuid.push_back(chars[dist(gen)]);
        }
    }
    return uuid; 
}

void cut_server_address(std::string &server_address) {
    if (server_address.rfind("http://", 0) == 0) {
        server_address = server_address.substr(7); 
    } 
    else if (server_address.rfind("https://", 0) == 0) {
        server_address = server_address.substr(8); 
    }
}

std::optional<std::string> deliver_message(db_manager &db, const std::string &sender_id, const std::string &recipient_id,
    const std::string &ciphertext, const std::string &plaintext, int64_t timestamp) {

    std::string message_id = generate_uuid();
    db_manager::message msg{message_id, sender_id, recipient_id, "", plaintext, ciphertext, false, timestamp, 0, 0, 1, 0};
    if(!db.add_message(msg)) {
        return std::nullopt;
    }

    std::string server_address = db.get_contact_address(recipient_id);
    cut_server_address(server_address); 

    crow::json::wvalue data_json;
    data_json["message_id"]   = message_id;
    data_json["sender_id"]    = sender_id;
    data_json["recipient_id"] = recipient_id;
    data_json["ciphertext"]         = ciphertext;
    data_json["timestamp"]    = timestamp;

    std::string url = "https://" + server_address + "/accept_message";
    auto result = send_message(url, data_json.dump());

    if (result.has_value() && *result == 200) {
        db.mark_accepted(message_id);
        return message_id;
    } 
    else {
        return std::nullopt; 
    }
}

bool deliver_message_edit(db_manager &db, const std::string &message_id, const std::string &sender_id,
    const std::string &recipient_id, const std::string &ciphertext, int64_t edited_at) {

    std::string server_address = db.get_contact_address(recipient_id);
    if (server_address.empty()) return false;
    cut_server_address(server_address); 

    crow::json::wvalue data_json;
    data_json["message_id"]   = message_id;
    data_json["sender_id"]    = sender_id;
    data_json["recipient_id"] = recipient_id;
    data_json["ciphertext"]   = ciphertext;
    data_json["edited_at"]    = edited_at;

    std::string url = "https://" + server_address + "/accept_edit";
    auto result = send_message(url, data_json.dump());

    bool delivered = result.has_value() && *result == 200;
    if (delivered) {
        db.mark_edit_accepted(message_id);
    }
    return delivered;
}

bool retry_deliver_message(db_manager &db, const db_manager::message &msg) {
    std::string server_address = db.get_contact_address(msg.recipient_id);
    if (server_address.empty()) return false;
    cut_server_address(server_address); 

    crow::json::wvalue data_json;
    data_json["message_id"] = msg.message_id;
    data_json["sender_id"] = msg.sender_id;
    data_json["recipient_id"] = msg.recipient_id;
    // data_json["plaintext"] = msg.plaintext;
    data_json["ciphertext"] = msg.ciphertext;
    data_json["timestamp"] = msg.timestamp;

    std::string url = "https://" + server_address + "/accept_message";
    auto result = send_message(url, data_json.dump());

    bool delivered = result.has_value() && *result == 200;
    if (delivered) {
        db.mark_accepted(msg.message_id);
    }
    return delivered;
}

bool retry_message_edit(db_manager &db, const db_manager::message &msg) {
    std::string server_address = db.get_contact_address(msg.recipient_id);
    if (server_address.empty()) return false;
    cut_server_address(server_address);

    crow::json::wvalue data_json;
    data_json["message_id"] = msg.message_id;
    data_json["sender_id"] = msg.sender_id;
    data_json["recipient_id"] = msg.recipient_id;
    data_json["ciphertext"] = msg.ciphertext;
    data_json["edited_at"] = msg.edited_at;

    std::string url = "https://" + server_address + "/accept_edit";
    auto result = send_message(url, data_json.dump());

    bool delivered = result.has_value() && *result == 200;
    if (delivered) {
        db.mark_edit_accepted(msg.message_id);
    }
    return delivered;
}

bool deliver_message_delete(db_manager &db, const std::string &message_id,
    const std::string &sender_id, const std::string &recipient_id) {
    std::string server_address = db.get_contact_address(recipient_id);
    if (server_address.empty()) return false;
    cut_server_address(server_address);

    crow::json::wvalue data_json;
    data_json["message_id"] = message_id;
    data_json["sender_id"] = sender_id;
    data_json["recipient_id"] = recipient_id;

    std::string url = "https://" + server_address + "/accept_delete";
    auto result = send_message(url, data_json.dump());

    bool delivered = result.has_value() && *result == 200;
    if (delivered) {
        db.mark_delete_accepted(message_id);
    }
    return delivered;
}

bool retry_message_delete(db_manager &db, const db_manager::message &msg) {
    std::string server_address = db.get_contact_address(msg.recipient_id);
    if (server_address.empty()) return false;
    cut_server_address(server_address);

    crow::json::wvalue data_json;
    data_json["message_id"] = msg.message_id;
    data_json["sender_id"] = msg.sender_id;
    data_json["recipient_id"] = msg.recipient_id;

    std::string url = "https://" + server_address + "/accept_delete";
    auto result = send_message(url, data_json.dump());

    bool delivered = result.has_value() && *result == 200;
    if (delivered) {
        db.mark_delete_accepted(msg.message_id);
    }
    return delivered;
}

bool deliver_group_update(db_manager &db, const std::string &group_id,
    const std::string &sender_id, const std::vector<db_manager::group_member> &members, int64_t version) {
    crow::json::wvalue snap;
    snap["group_id"] = group_id;
    snap["sender_id"] = sender_id;
    snap["version"] = version;
    std::vector<crow::json::wvalue> ml;
    for (const auto &m : members) {
        crow::json::wvalue e;
        e["member_id"] = m.member_id;
        e["server_address"] = m.server_address;
        e["role"] = m.role;
        e["added_at"] = m.added_at;
        ml.push_back(std::move(e));
    }
    snap["members"] = std::move(ml);
    std::string snapshot_json = snap.dump();

    bool all_online = true;
    for (const auto &m : members) {
        if (m.member_id == sender_id) continue;
        db_manager::group_update u;
        u.group_id = group_id;
        u.member_id = m.member_id;
        u.snapshot_json = snapshot_json;
        u.version = version;
        db.upsert_group_update(u);

        std::string addr = m.server_address;
        cut_server_address(addr);
        if (addr.empty()) { all_online = false; continue; }

        std::string url = "https://" + addr + "/accept_group_update";
        auto res = send_message(url, snapshot_json);
        bool ok = res.has_value() && *res == 200;
        if (ok) {
            db.mark_group_update_accepted(group_id, m.member_id);
        } else {
            all_online = false;
        }
    }
    return all_online;
}

bool retry_group_update(db_manager &db, const db_manager::group_update &u) {
    std::string addr = db.get_contact_address(u.member_id);
    if (addr.empty()) return false;
    cut_server_address(addr);

    std::string url = "https://" + addr + "/accept_group_update";
    auto res = send_message(url, u.snapshot_json);
    bool ok = res.has_value() && *res == 200;
    if (ok) db.mark_group_update_accepted(u.group_id, u.member_id);
    return ok;
}
