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

std::optional<std::string> deliver_message(db_manager &db, int64_t sender_id, int64_t recipient_id, 
    const std::string &text, int64_t timestamp) {

    std::string message_id = generate_uuid();
    db_manager::message msg{message_id, sender_id, recipient_id, text, false, timestamp};
    if(!db.add_message(msg)) {
        return std::nullopt;
    }

    std::string server_address = db.get_contact_address(recipient_id);
    // if(server_address.empty()) {
    //     return std::nullopt;
    // }

    crow::json::wvalue data_json;
    data_json["message_id"]   = message_id;
    data_json["sender_id"]    = sender_id;
    data_json["recipient_id"] = recipient_id;
    data_json["text"]         = text;
    data_json["timestamp"]    = timestamp;

    std::string url = "http://" + server_address + "/accept_message";
    auto result = send_message(url, data_json.dump());

    if (result.has_value() && *result == 200) {
        db.mark_accepted(message_id);
    }

    return message_id;
}

bool retry_deliver_message(db_manager &db, const db_manager::message &msg) {
    std::string server_address = db.get_contact_address(msg.recipient_id);
    if (server_address.empty()) return false;

    crow::json::wvalue data_json;
    data_json["message_id"] = msg.message_id;
    data_json["sender_id"] = msg.sender_id;
    data_json["recipient_id"] = msg.recipient_id;
    data_json["text"] = msg.text;
    data_json["timestamp"] = msg.timestamp;

    std::string url = "http://" + server_address + "/accept_message";
    auto result = send_message(url, data_json.dump());

    bool delivered = result.has_value() && *result == 200;
    if (delivered) {
        db.mark_accepted(msg.message_id);
    }
    return delivered;
}