#include "db_manager.h"
#include <stdexcept>
#include <iostream>
#include <optional>

db_manager::db_manager(const std::string &db_path) {
    sqlite3 *raw_db = nullptr;
    if (sqlite3_open(db_path.c_str(), &raw_db) != SQLITE_OK) {
        const char* error_msg = "unknown error";
        if (raw_db) error_msg = sqlite3_errmsg(raw_db);
        throw std::runtime_error(
            "[SQL] Error opening database " + db_path + ": " + error_msg
        );
    }
    db_.reset(raw_db);
    sqlite3_exec(db_.get(), "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    init_schema();
    std::cout << "[SQL] Database " << db_path << " was opened successfully.\n";
}

void db_manager::init_schema() {
    const char *sql_create = R"(
        CREATE TABLE IF NOT EXISTS contacts (
            contact_id      TEXT PRIMARY KEY, -- public_key
            name            TEXT NOT NULL,
            server_address  TEXT
        );

        CREATE TABLE IF NOT EXISTS messages (
            message_id      TEXT PRIMARY KEY, -- UUID
            sender_id       TEXT NOT NULL, -- public_key
            recipient_id    TEXT NOT NULL, -- public_key
            plaintext            TEXT NOT NULL,
            ciphertext            TEXT NOT NULL,
            accepted        INTEGER NOT NULL DEFAULT 0,
            timestamp       INTEGER NOT NULL DEFAULT (unixepoch())
        );

        CREATE TABLE IF NOT EXISTS clients (
            client_id   TEXT PRIMARY KEY,
            device_name TEXT NOT NULL DEFAULT '',
            salt        TEXT NOT NULL,
            secret_hash TEXT NOT NULL,
            created_at  INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS auth_requests (
            request_id       TEXT PRIMARY KEY,
            device_name      TEXT NOT NULL,
            created_at       INTEGER NOT NULL,
            expires_at       INTEGER NOT NULL,
            approved_client_id TEXT,
            approved_secret  TEXT
        );
    )";

    char *errMsg = nullptr;
    int rc = sqlite3_exec(db_.get(), sql_create, nullptr, nullptr, &errMsg);

    if (rc != SQLITE_OK) {
        std::string error = errMsg ? errMsg : "unknown error";
        sqlite3_free(errMsg);
        throw std::runtime_error("[SQL] Error creating schema: " + error);
    }
    sqlite3_exec(db_.get(),
        "ALTER TABLE clients ADD COLUMN device_name TEXT NOT NULL DEFAULT '';",
        nullptr, nullptr, nullptr);
    std::cout << "[SQL] Schema ready.\n";
}

bool db_manager::upsert_contact(const std::string &contact_id, const std::string &name, const std::string &server_address) {
    const char *sql_insert =
        "INSERT INTO contacts (contact_id, name, server_address) "
        "VALUES (?, ?, ?) "
        "ON CONFLICT (contact_id) "
        "DO UPDATE SET name = EXCLUDED.name, server_address = EXCLUDED.server_address;"; 

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), sql_insert, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare insert: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, contact_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, server_address.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    bool ok = (rc == SQLITE_DONE);
    if (!ok) {
        std::cerr << "[SQL] Contact insert failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }

    sqlite3_finalize(stmt);
    return ok;
}

bool db_manager::add_message(const message &msg) {
    const char *sql_insert =
        "INSERT INTO messages (message_id, sender_id, recipient_id, plaintext, ciphertext, accepted, timestamp) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), sql_insert, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare insert: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, msg.message_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, msg.sender_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, msg.recipient_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, msg.plaintext.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, msg.ciphertext.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, msg.accepted);
    sqlite3_bind_int64(stmt, 7, msg.timestamp);

    rc = sqlite3_step(stmt);
    bool ok = (rc == SQLITE_DONE);
    if (!ok) {
        std::cerr << "[SQL] Message insert failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }

    sqlite3_finalize(stmt);
    return ok;
}

void db_manager::mark_accepted(const std::string &message_id) {
    const char *sql = "UPDATE messages SET accepted = 1 WHERE message_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare update: " << sqlite3_errmsg(db_.get()) << std::endl;
        return;
    }
    sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "[SQL] Update failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }
    sqlite3_finalize(stmt);
}

void db_manager::mark_failed(const std::string &message_id) {
    const char *sql = "UPDATE messages SET accepted = 2 WHERE message_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare update: " << sqlite3_errmsg(db_.get()) << std::endl;
        return;
    }
    sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "[SQL] Update failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }
    sqlite3_finalize(stmt);
}

std::string db_manager::get_contact_address(const std::string& contact_id) {
    const char *sql = "SELECT server_address FROM contacts WHERE contact_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    std::string address = "";

    if(sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare select: " << sqlite3_errmsg(db_.get()) << std::endl;
        return address;
    }
    sqlite3_bind_text(stmt, 1, contact_id.c_str(), -1, SQLITE_TRANSIENT);

    if(sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        if(text) address = reinterpret_cast<const char*>(text);
    }
    sqlite3_finalize(stmt);
    return address;
}

std::vector<db_manager::message> db_manager::get_pending_messages() {
    const char *sql =
        "SELECT message_id, sender_id, recipient_id, plaintext, ciphertext, accepted, timestamp "
        "FROM messages WHERE accepted = 0;";
    sqlite3_stmt* stmt = nullptr;
    std::vector<message> result;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare select pending: " << sqlite3_errmsg(db_.get()) << std::endl;
        return result;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        message msg;
        msg.message_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        msg.sender_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        msg.recipient_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        msg.plaintext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        msg.ciphertext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        msg.accepted = sqlite3_column_int(stmt, 5);
        msg.timestamp = sqlite3_column_int64(stmt, 6);
        result.push_back(std::move(msg));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<db_manager::contact_info> db_manager::get_contacts_with_latest_message(const std::string& self_id) {
    std::vector<contact_info> result;
    const char *sql_contacts = "SELECT contact_id, name, server_address FROM contacts;";
    sqlite3_stmt* stmt_contacts = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql_contacts, -1, &stmt_contacts, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare contacts select: " << sqlite3_errmsg(db_.get()) << std::endl;
        return result;
    }

    while (sqlite3_step(stmt_contacts) == SQLITE_ROW) {
        contact_info info;
        info.contact_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt_contacts, 0));
        info.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt_contacts, 1));
        info.server_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt_contacts, 2));
        const char *sql_latest =
            "SELECT message_id, sender_id, recipient_id, plaintext, ciphertext, accepted, timestamp "
            "FROM messages "
            "WHERE ((sender_id = ? AND recipient_id = ?) OR (sender_id = ? AND recipient_id = ?)) "
            "ORDER BY timestamp DESC LIMIT 1;";
        sqlite3_stmt* stmt_latest = nullptr;
        if (sqlite3_prepare_v2(db_.get(), sql_latest, -1, &stmt_latest, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt_latest, 1, self_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt_latest, 2, info.contact_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt_latest, 3, info.contact_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt_latest, 4, self_id.c_str(), -1, SQLITE_TRANSIENT);

            if (sqlite3_step(stmt_latest) == SQLITE_ROW) {
                message msg;
                msg.message_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt_latest, 0));
                msg.sender_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt_latest, 1));
                msg.recipient_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt_latest, 2));
                msg.plaintext = reinterpret_cast<const char*>(sqlite3_column_text(stmt_latest, 3));
                msg.ciphertext = reinterpret_cast<const char*>(sqlite3_column_text(stmt_latest, 4));
                msg.accepted = sqlite3_column_int(stmt_latest, 5);
                msg.timestamp = sqlite3_column_int64(stmt_latest, 6);
                info.latest_message = std::move(msg);
            }
            sqlite3_finalize(stmt_latest);
        } else {
            std::cerr << "[SQL] Failed to prepare latest message select: " << sqlite3_errmsg(db_.get()) << std::endl;
        }

        result.push_back(std::move(info));
    }
    sqlite3_finalize(stmt_contacts);
    return result;
}

std::vector<db_manager::message> db_manager::get_messages_between(const std::string& self_id, const std::string& other_id) {
    std::vector<message> result;
    const char *sql =
        "SELECT message_id, sender_id, recipient_id, plaintext, ciphertext, accepted, timestamp "
        "FROM messages "
        "WHERE ((sender_id = ? AND recipient_id = ?) OR (sender_id = ? AND recipient_id = ?)) "
        "ORDER BY timestamp ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare messages select: " << sqlite3_errmsg(db_.get()) << std::endl;
        return result;
    }
    sqlite3_bind_text(stmt, 1, self_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, other_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, other_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, self_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        message msg;
        msg.message_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        msg.sender_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        msg.recipient_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        msg.plaintext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        msg.ciphertext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        msg.accepted = sqlite3_column_int(stmt, 5);
        msg.timestamp = sqlite3_column_int64(stmt, 6);
        result.push_back(std::move(msg));
    }
    sqlite3_finalize(stmt);
    return result;
}

bool db_manager::has_clients() {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), "SELECT COUNT(*) FROM clients;", -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare clients count: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }
    bool empty = true;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        empty = sqlite3_column_int(stmt, 0) == 0;
    }
    sqlite3_finalize(stmt);
    return !empty;
}

bool db_manager::create_client(const std::string &client_id, const std::string &device_name,
                            const std::string &salt,
                            const std::string &secret_hash, int64_t created_at) {
    const char *sql = "INSERT INTO clients (client_id, device_name, salt, secret_hash, created_at) VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare client insert: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, client_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, device_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, salt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, secret_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, created_at);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool db_manager::get_client(const std::string &client_id, client &out) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(),
        "SELECT client_id, device_name, salt, secret_hash, created_at FROM clients WHERE client_id = ?;",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare client select: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, client_id.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out.client_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        out.device_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        out.salt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        out.secret_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        out.created_at = sqlite3_column_int64(stmt, 4);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool db_manager::delete_client(const std::string &client_id) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), "DELETE FROM clients WHERE client_id = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare client delete: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, client_id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<db_manager::client> db_manager::clients_list() {
    std::vector<client> result;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(),
        "SELECT client_id, device_name, salt, secret_hash, created_at FROM clients ORDER BY created_at ASC;",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare clients list: " << sqlite3_errmsg(db_.get()) << std::endl;
        return result;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        client c;
        c.client_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        c.device_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        c.salt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        c.secret_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        c.created_at = sqlite3_column_int64(stmt, 4);
        result.push_back(std::move(c));
    }
    sqlite3_finalize(stmt);
    return result;
}

bool db_manager::add_auth_request(const std::string &request_id, const std::string &device_name,
                                int64_t created_at, int64_t expires_at) {
    const char *sql = "INSERT INTO auth_requests "
        "(request_id, device_name, created_at, expires_at, approved_client_id, approved_secret) "
        "VALUES (?, ?, ?, ?, NULL, NULL);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare auth request insert: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, request_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, device_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, created_at);
    sqlite3_bind_int64(stmt, 4, expires_at);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool db_manager::get_auth_request(const std::string &request_id, auth_request &out) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(),
        "SELECT request_id, device_name, created_at, expires_at, approved_client_id, approved_secret "
        "FROM auth_requests WHERE request_id = ?;",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare auth request select: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, request_id.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out.request_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        out.device_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        out.created_at = sqlite3_column_int64(stmt, 2);
        out.expires_at = sqlite3_column_int64(stmt, 3);
        const char* cid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const char* sec = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        if (cid) out.approved_client_id = cid;
        if (sec) out.approved_secret = sec;
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool db_manager::delete_auth_request(const std::string &request_id) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), "DELETE FROM auth_requests WHERE request_id = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare auth request delete: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, request_id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool db_manager::approve_auth_request(const std::string &request_id,
                                    const std::string &approved_client_id,
                                    const std::string &approved_secret) {
    const char *sql = "UPDATE auth_requests "
        "SET approved_client_id = ?, approved_secret = ? WHERE request_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare auth request approve: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, approved_client_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, approved_secret.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, request_id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}