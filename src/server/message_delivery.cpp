#include "message_delivery.h"
#include "http_client.h"
#include <crow.h>
#include <random>
#include <sstream>
#include <iomanip>

std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;
    uint64_t a = dist(gen);
    return std::to_string(a); 
}

std::string deliver_message(db_manager &db, int64_t sender_id, int64_t recipient_id, 
    const std::string &text, int64_t timestamp) {

    std::string message_id = generate_uuid();
    db_manager::message msg{message_id, sender_id, recipient_id, text, false, timestamp};
    if(!db.add_message(msg)) {
        return message_id; 
    }

    std::string server_address = db.get_contact_address(recipient_id);
    if(server_address.empty()) {
        return message_id;
    }

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