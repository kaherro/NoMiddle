#pragma once
#include <sqlite3.h>
#include <string>
#include <memory>

class db_manager {
public:
    explicit db_manager(const std::string &db_path = "NoMiddle.db");

    struct message {
        std::string message_id; // UUID
        int64_t sender_id;
        int64_t recipient_id;
        std::string text;
        bool accepted;
        int64_t timestamp; // unix-time
    };
    
    bool add_contact(const std::string &name, const std::string &server_address, const std::string &public_key); 
    bool add_message(const message &msg);
    void mark_accepted(const std::string &message_id);
    std::string get_contact_address(int64_t contact_id);

private:
    struct sqlite3_deleter {
        void operator()(sqlite3* db) const {
            sqlite3_close(db);
        }
    };
    std::unique_ptr<sqlite3, sqlite3_deleter> db_;

    void init_schema();
};