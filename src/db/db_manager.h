#pragma once
#include <sqlite3.h>
#include <string>
#include <memory>
#include <vector>

constexpr int MESSAGE_PENDING   = 0; 
constexpr int MESSAGE_DELIVERED = 1;
constexpr int MESSAGE_FAILED    = 2;

class db_manager {
public:
    explicit db_manager(const std::string &db_path = "NoMiddle.db");

    struct message {
        std::string message_id; // UUID
        std::string sender_id;
        std::string recipient_id;
        std::string text;
        int accepted;
        int64_t timestamp; // unix-time
    };
    bool add_contact(const std::string &contact_id, const std::string &name, const std::string &server_address);
    bool add_message(const message &msg);
    void mark_accepted(const std::string &message_id);
    void mark_failed(const std::string &message_id);
    std::string get_contact_address(const std::string& contact_id);
    std::vector<message> get_pending_messages();

private:
    struct sqlite3_deleter {
        void operator()(sqlite3* db) const {
            sqlite3_close(db);
        }
    };
    std::unique_ptr<sqlite3, sqlite3_deleter> db_;

    void init_schema();
};