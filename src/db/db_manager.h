#pragma once
#include <sqlite3.h>
#include <string>
#include <memory>
#include <vector>
#include <optional>

constexpr int MESSAGE_PENDING   = 0; 
constexpr int MESSAGE_DELIVERED = 1;
constexpr int MESSAGE_FAILED    = 2;
constexpr int MESSAGE_DELETED   = 3;

class db_manager {
public:
    explicit db_manager(const std::string &db_path = "NoMiddle.db");

    struct message {
        std::string message_id; // UUID
        std::string sender_id;
        std::string recipient_id;
        std::string plaintext;
        std::string ciphertext; 
        int accepted;
        int64_t timestamp; // unix-time
        int64_t edited_at; // 0 if never edited
        int edit_accepted;
    };

    bool upsert_contact(const std::string &contact_id, const std::string &name, const std::string &server_address);
    bool add_message(const message &msg);
    void mark_accepted(const std::string &message_id);
    void mark_failed(const std::string &message_id);
    void mark_deleted(const std::string &message_id);
    void mark_edit_accepted(const std::string &message_id);
    void mark_edit_failed(const std::string &message_id);
    bool get_message(const std::string &message_id, message &out);
    bool update_message_edit(const std::string &message_id, const std::string &plaintext,
                            const std::string &ciphertext, int64_t edited_at);
    std::string get_contact_address(const std::string& contact_id);
    std::vector<message> get_pending_messages();
    std::vector<message> get_pending_edits();

    struct contact_info {
        std::string contact_id;
        std::string name;
        std::string server_address;
        std::optional<message> latest_message; 
    };
    std::vector<contact_info> get_contacts_with_latest_message(const std::string& self_id);
    std::vector<message> get_messages_between(const std::string& self_id, const std::string& other_id);

    struct client {
        std::string client_id;
        std::string device_name;
        std::string salt;
        std::string secret_hash;
        int64_t created_at;
    };

    bool has_clients();
    bool create_client(const std::string &client_id, const std::string &device_name,
                    const std::string &salt,
                    const std::string &secret_hash, int64_t created_at);
    bool get_client(const std::string &client_id, client &out);
    bool delete_client(const std::string &client_id);
    std::vector<client> clients_list();

    struct auth_request {
        std::string request_id;
        std::string device_name;
        int64_t created_at;
        int64_t expires_at;
        std::string approved_client_id;
        std::string approved_secret;
    };

    bool add_auth_request(const std::string &request_id, const std::string &device_name,
                        int64_t created_at, int64_t expires_at);
    bool get_auth_request(const std::string &request_id, auth_request &out);
    bool delete_auth_request(const std::string &request_id);
    bool approve_auth_request(const std::string &request_id,
                            const std::string &approved_client_id,
                            const std::string &approved_secret);

private:
    struct sqlite3_deleter {
        void operator()(sqlite3* db) const {
            sqlite3_close(db);
        }
    };
    std::unique_ptr<sqlite3, sqlite3_deleter> db_;

    void init_schema();
};