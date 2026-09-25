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
            group_id        TEXT, -- group public_key; NULL for direct messages
            plaintext            TEXT NOT NULL,
            ciphertext            TEXT NOT NULL,
            accepted        INTEGER NOT NULL DEFAULT 0,
            timestamp       INTEGER NOT NULL DEFAULT (unixepoch()),
            edited_at       INTEGER NOT NULL DEFAULT 0,
            edit_accepted   INTEGER NOT NULL DEFAULT 0,
            delete_accepted INTEGER NOT NULL DEFAULT 1,
            deleted_at      INTEGER NOT NULL DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS groups (
            group_id        TEXT PRIMARY KEY,
            name            TEXT NOT NULL,
            created_by      TEXT NOT NULL,
            created_at      INTEGER NOT NULL DEFAULT (unixepoch())
        );

        CREATE TABLE IF NOT EXISTS group_members (
            group_id        TEXT NOT NULL,
            member_id       TEXT NOT NULL, -- public_key
            server_address  TEXT NOT NULL,
            role            TEXT NOT NULL DEFAULT 'member',
            added_at        INTEGER NOT NULL DEFAULT (unixepoch()),
            PRIMARY KEY (group_id, member_id)
        );

        CREATE INDEX IF NOT EXISTS idx_messages_group ON messages(group_id);
        CREATE INDEX IF NOT EXISTS idx_group_members_group ON group_members(group_id);

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

        CREATE TABLE IF NOT EXISTS group_updates (
            group_id       TEXT NOT NULL,
            member_id      TEXT NOT NULL,
            snapshot_json  TEXT NOT NULL, 
            version        INTEGER NOT NULL DEFAULT 0,
            accepted       INTEGER NOT NULL DEFAULT 0, -- 0 pending, 1 accepted
            created_at     INTEGER NOT NULL DEFAULT (unixepoch()),
            PRIMARY KEY (group_id, member_id)
        );

        CREATE INDEX IF NOT EXISTS idx_group_updates_pending
            ON group_updates(accepted);
    )";

    char *errMsg = nullptr;
    int rc = sqlite3_exec(db_.get(), sql_create, nullptr, nullptr, &errMsg);

    if (rc != SQLITE_OK) {
        std::string error = errMsg ? errMsg : "unknown error";
        sqlite3_free(errMsg);
        throw std::runtime_error("[SQL] Error creating schema: " + error);
    }

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
        "INSERT INTO messages (message_id, sender_id, recipient_id, group_id, plaintext, ciphertext, accepted, timestamp) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_.get(), sql_insert, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare insert: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, msg.message_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, msg.sender_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, msg.recipient_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, msg.group_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, msg.plaintext.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, msg.ciphertext.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, msg.accepted);
    sqlite3_bind_int64(stmt, 8, msg.timestamp);

    rc = sqlite3_step(stmt);
    bool ok = (rc == SQLITE_DONE);
    if (!ok) {
        std::cerr << "[SQL] Message insert failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }

    sqlite3_finalize(stmt);
    return ok;
}

void db_manager::mark_accepted(const std::string &message_id, const std::string &recipient_id) {
    const char *sql = "UPDATE messages SET accepted = 1 WHERE message_id = ? AND recipient_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare update: " << sqlite3_errmsg(db_.get()) << std::endl;
        return;
    }
    sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, recipient_id.c_str(), -1, SQLITE_TRANSIENT);
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

void db_manager::mark_deleted(const std::string &message_id) {
    const char *sql = "UPDATE messages SET accepted = 3 WHERE message_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare delete update: " << sqlite3_errmsg(db_.get()) << std::endl;
        return;
    }
    sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "[SQL] Update failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }
    sqlite3_finalize(stmt);
}

void db_manager::mark_delete_pending(const std::string &message_id) {
    const char *sql = "UPDATE messages SET delete_accepted = 0, deleted_at = unixepoch() WHERE message_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare delete pending update: " << sqlite3_errmsg(db_.get()) << std::endl;
        return;
    }
    sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "[SQL] Update failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }
    sqlite3_finalize(stmt);
}

void db_manager::mark_delete_accepted(const std::string &message_id) {
    const char *sql = "UPDATE messages SET delete_accepted = 1 WHERE message_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare delete accept update: " << sqlite3_errmsg(db_.get()) << std::endl;
        return;
    }
    sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "[SQL] Update failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }
    sqlite3_finalize(stmt);
}

void db_manager::mark_delete_accepted(const std::string &message_id, const std::string &recipient_id) {
    const char *sql = "UPDATE messages SET delete_accepted = 1 WHERE message_id = ? AND recipient_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare delete accept update: " << sqlite3_errmsg(db_.get()) << std::endl;
        return;
    }
    sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, recipient_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "[SQL] Update failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }
    sqlite3_finalize(stmt);
}

void db_manager::mark_delete_failed(const std::string &message_id) {
    const char *sql = "UPDATE messages SET delete_accepted = 2 WHERE message_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare delete failed update: " << sqlite3_errmsg(db_.get()) << std::endl;
        return;
    }
    sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "[SQL] Update failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }
    sqlite3_finalize(stmt);
}

void db_manager::mark_edit_accepted(const std::string &message_id) {
    const char *sql = "UPDATE messages SET edit_accepted = 1 WHERE message_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare edit update: " << sqlite3_errmsg(db_.get()) << std::endl;
        return;
    }
    sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "[SQL] Update failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }
    sqlite3_finalize(stmt);
}

void db_manager::mark_edit_accepted(const std::string &message_id, const std::string &recipient_id) {
    const char *sql = "UPDATE messages SET edit_accepted = 1 WHERE message_id = ? AND recipient_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare edit update: " << sqlite3_errmsg(db_.get()) << std::endl;
        return;
    }
    sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, recipient_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "[SQL] Update failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }
    sqlite3_finalize(stmt);
}

void db_manager::mark_edit_failed(const std::string &message_id) {
    const char *sql = "UPDATE messages SET edit_accepted = 2 WHERE message_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare edit update: " << sqlite3_errmsg(db_.get()) << std::endl;
        return;
    }
    sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "[SQL] Update failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }
    sqlite3_finalize(stmt);
}

bool db_manager::get_message(const std::string &message_id, message &out) {
    const char *sql =
        "SELECT message_id, sender_id, recipient_id, group_id, plaintext, ciphertext, accepted, timestamp, edited_at, edit_accepted, delete_accepted, deleted_at "
        "FROM messages WHERE message_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare message select: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out.message_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        out.sender_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        out.recipient_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        out.group_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        out.plaintext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        out.ciphertext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        out.accepted = sqlite3_column_int(stmt, 6);
        out.timestamp = sqlite3_column_int64(stmt, 7);
        out.edited_at = sqlite3_column_int64(stmt, 8);
        out.edit_accepted = sqlite3_column_int(stmt, 9);
        out.delete_accepted = sqlite3_column_int(stmt, 10);
        out.deleted_at = sqlite3_column_int64(stmt, 11);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool db_manager::update_message_edit(const std::string &message_id, const std::string &plaintext,
                                    const std::string &ciphertext, int64_t edited_at) {
    const char *sql = "UPDATE messages SET plaintext = ?, ciphertext = ?, edited_at = ?, edit_accepted = 0 WHERE message_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare message edit: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, plaintext.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ciphertext.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, edited_at);
    sqlite3_bind_text(stmt, 4, message_id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!ok) {
        std::cerr << "[SQL] Message edit failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool db_manager::update_message_edit_for(const std::string &message_id, const std::string &recipient_id,
                                        const std::string &plaintext, const std::string &ciphertext, int64_t edited_at) {
    const char *sql = "UPDATE messages SET plaintext = ?, ciphertext = ?, edited_at = ?, edit_accepted = 0 WHERE message_id = ? AND recipient_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare message edit: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, plaintext.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ciphertext.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, edited_at);
    sqlite3_bind_text(stmt, 4, message_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, recipient_id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!ok) {
        std::cerr << "[SQL] Message edit failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }
    sqlite3_finalize(stmt);
    return ok;
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

std::string db_manager::get_contact_name(const std::string& contact_id) {
    const char *sql = "SELECT name FROM contacts WHERE contact_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    std::string name = "";

    if(sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare select: " << sqlite3_errmsg(db_.get()) << std::endl;
        return name;
    }
    sqlite3_bind_text(stmt, 1, contact_id.c_str(), -1, SQLITE_TRANSIENT);

    if(sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        if(text) name = reinterpret_cast<const char*>(text);
    }
    sqlite3_finalize(stmt);
    return name;
}

std::vector<db_manager::message> db_manager::get_pending_messages() {
    const char *sql =
        "SELECT message_id, sender_id, recipient_id, group_id, plaintext, ciphertext, accepted, timestamp, edited_at, edit_accepted, delete_accepted, deleted_at "
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
        msg.group_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        msg.plaintext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        msg.ciphertext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        msg.accepted = sqlite3_column_int(stmt, 6);
        msg.timestamp = sqlite3_column_int64(stmt, 7);
        msg.edited_at = sqlite3_column_int64(stmt, 8);
        msg.edit_accepted = sqlite3_column_int(stmt, 9);
        msg.delete_accepted = sqlite3_column_int(stmt, 10);
        msg.deleted_at = sqlite3_column_int64(stmt, 11);
        result.push_back(std::move(msg));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<db_manager::message> db_manager::get_pending_edits() {
    const char *sql =
        "SELECT message_id, sender_id, recipient_id, group_id, plaintext, ciphertext, accepted, timestamp, edited_at, edit_accepted, delete_accepted, deleted_at "
        "FROM messages WHERE edit_accepted = 0 AND edited_at != 0;";
    sqlite3_stmt* stmt = nullptr;
    std::vector<message> result;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare select pending edits: " << sqlite3_errmsg(db_.get()) << std::endl;
        return result;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        message msg;
        msg.message_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        msg.sender_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        msg.recipient_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        msg.group_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        msg.plaintext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        msg.ciphertext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        msg.accepted = sqlite3_column_int(stmt, 6);
        msg.timestamp = sqlite3_column_int64(stmt, 7);
        msg.edited_at = sqlite3_column_int64(stmt, 8);
        msg.edit_accepted = sqlite3_column_int(stmt, 9);
        msg.delete_accepted = sqlite3_column_int(stmt, 10);
        msg.deleted_at = sqlite3_column_int64(stmt, 11);
        result.push_back(std::move(msg));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<db_manager::message> db_manager::get_pending_deletes() {
    const char *sql =
        "SELECT message_id, sender_id, recipient_id, group_id, plaintext, ciphertext, accepted, timestamp, edited_at, edit_accepted, delete_accepted, deleted_at "
        "FROM messages WHERE delete_accepted = 0 AND accepted = 3;";
    sqlite3_stmt* stmt = nullptr;
    std::vector<message> result;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare select pending deletes: " << sqlite3_errmsg(db_.get()) << std::endl;
        return result;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        message msg;
        msg.message_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        msg.sender_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        msg.recipient_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        msg.group_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        msg.plaintext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        msg.ciphertext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        msg.accepted = sqlite3_column_int(stmt, 6);
        msg.timestamp = sqlite3_column_int64(stmt, 7);
        msg.edited_at = sqlite3_column_int64(stmt, 8);
        msg.edit_accepted = sqlite3_column_int(stmt, 9);
        msg.delete_accepted = sqlite3_column_int(stmt, 10);
        msg.deleted_at = sqlite3_column_int64(stmt, 11);
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
            "SELECT message_id, sender_id, recipient_id, group_id, plaintext, ciphertext, accepted, timestamp, edited_at, edit_accepted, delete_accepted, deleted_at "
            "FROM messages "
            "WHERE ((sender_id = ? AND recipient_id = ?) OR (sender_id = ? AND recipient_id = ?)) AND group_id = '' AND accepted != 3 "
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
                msg.group_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt_latest, 3));
                msg.plaintext = reinterpret_cast<const char*>(sqlite3_column_text(stmt_latest, 4));
                msg.ciphertext = reinterpret_cast<const char*>(sqlite3_column_text(stmt_latest, 5));
                msg.accepted = sqlite3_column_int(stmt_latest, 6);
                msg.timestamp = sqlite3_column_int64(stmt_latest, 7);
                msg.edited_at = sqlite3_column_int64(stmt_latest, 8);
                msg.edit_accepted = sqlite3_column_int(stmt_latest, 9);
                msg.delete_accepted = sqlite3_column_int(stmt_latest, 10);
                msg.deleted_at = sqlite3_column_int64(stmt_latest, 11);
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
        "SELECT message_id, sender_id, recipient_id, group_id, plaintext, ciphertext, accepted, timestamp, edited_at, edit_accepted, delete_accepted, deleted_at "
        "FROM messages "
        "WHERE ((sender_id = ? AND recipient_id = ?) OR (sender_id = ? AND recipient_id = ?)) AND group_id = '' AND accepted != 3 "
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
        msg.group_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        msg.plaintext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        msg.ciphertext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        msg.accepted = sqlite3_column_int(stmt, 6);
        msg.timestamp = sqlite3_column_int64(stmt, 7);
        msg.edited_at = sqlite3_column_int64(stmt, 8);
        msg.edit_accepted = sqlite3_column_int(stmt, 9);
        msg.delete_accepted = sqlite3_column_int(stmt, 10);
        msg.deleted_at = sqlite3_column_int64(stmt, 11);
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

std::vector<db_manager::group> db_manager::get_groups() {
    std::vector<group> result;
    const char *sql = "SELECT group_id, name, created_by, created_at FROM groups ORDER BY created_at DESC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare groups list: " << sqlite3_errmsg(db_.get()) << std::endl;
        return result;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        group g;
        g.group_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        g.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        g.created_by = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        g.created_at = sqlite3_column_int64(stmt, 3);
        result.push_back(std::move(g));
    }
    sqlite3_finalize(stmt);
    return result;
}

bool db_manager::create_group(const group &g) {
    const char *sql = "INSERT INTO groups (group_id, name, created_by, created_at) "
        "VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare group insert: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, g.group_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, g.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, g.created_by.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, g.created_at);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!ok) {
        std::cerr << "[SQL] Group insert failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool db_manager::update_group_name(const std::string &group_id, const std::string &name) {
    const char *sql = "UPDATE groups SET name = ? WHERE group_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare group name update: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, group_id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool db_manager::get_group(const std::string &group_id, group &out) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(),
        "SELECT group_id, name, created_by, created_at FROM groups WHERE group_id = ?;",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare group select: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out.group_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        out.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        out.created_by = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        out.created_at = sqlite3_column_int64(stmt, 3);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

void db_manager::delete_group(const std::string &group_id) {
    const char *sql_groups = "DELETE FROM groups WHERE group_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql_groups, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);

    const char *sql_members = "DELETE FROM group_members WHERE group_id = ?;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql_members, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

bool db_manager::replace_group_members(const std::string &group_id, const std::vector<group_member> &members) {
    const char *sql_delete = "DELETE FROM group_members WHERE group_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql_delete, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare group members delete: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    if (!ok) {
        std::cerr << "[SQL] Group members delete failed: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }

    const char *sql_insert = "INSERT INTO group_members (group_id, member_id, server_address, role, added_at) "
        "VALUES (?, ?, ?, ?, ?);";
    bool all_ok = true;
    for (const auto &m : members) {
        stmt = nullptr;
        if (sqlite3_prepare_v2(db_.get(), sql_insert, -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "[SQL] Failed to prepare group member insert: " << sqlite3_errmsg(db_.get()) << std::endl;
            all_ok = false;
            continue;
        }
        sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, m.member_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, m.server_address.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, m.role.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, m.added_at);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::cerr << "[SQL] Group member insert failed: " << sqlite3_errmsg(db_.get()) << std::endl;
            all_ok = false;
        }
        sqlite3_finalize(stmt);
    }
    return all_ok;
}

std::vector<db_manager::group_member> db_manager::get_group_members(const std::string &group_id) {
    std::vector<group_member> result;
    const char *sql = "SELECT group_id, member_id, server_address, role, added_at "
        "FROM group_members WHERE group_id = ? ORDER BY added_at ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare group members select: " << sqlite3_errmsg(db_.get()) << std::endl;
        return result;
    }
    sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        group_member m;
        m.group_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        m.member_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        m.server_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        m.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        m.added_at = sqlite3_column_int64(stmt, 4);
        result.push_back(std::move(m));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<db_manager::message> db_manager::get_messages_for_group(const std::string &group_id) {
    std::vector<message> result;
    const char *sql =
        "SELECT message_id, sender_id, recipient_id, group_id, plaintext, ciphertext, accepted, timestamp, edited_at, edit_accepted, delete_accepted, deleted_at "
        "FROM messages WHERE group_id = ? AND accepted != 3 "
        "ORDER BY timestamp ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare group messages select: " << sqlite3_errmsg(db_.get()) << std::endl;
        return result;
    }
    sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        message msg;
        msg.message_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        msg.sender_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        msg.recipient_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        msg.group_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        msg.plaintext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        msg.ciphertext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        msg.accepted = sqlite3_column_int(stmt, 6);
        msg.timestamp = sqlite3_column_int64(stmt, 7);
        msg.edited_at = sqlite3_column_int64(stmt, 8);
        msg.edit_accepted = sqlite3_column_int(stmt, 9);
        msg.delete_accepted = sqlite3_column_int(stmt, 10);
        msg.deleted_at = sqlite3_column_int64(stmt, 11);
        result.push_back(std::move(msg));
    }
    sqlite3_finalize(stmt);
    return result;
}

bool db_manager::get_last_message_for_group(const std::string &group_id, message &out) {
    const char *sql =
        "SELECT message_id, sender_id, recipient_id, group_id, plaintext, ciphertext, accepted, timestamp, edited_at, edit_accepted, delete_accepted, deleted_at "
        "FROM messages WHERE group_id = ? AND accepted != 3 "
        "ORDER BY timestamp DESC LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare last group message select: " << sqlite3_errmsg(db_.get()) << std::endl;
        return false;
    }
    sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out.message_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        out.sender_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        out.recipient_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        out.group_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        out.plaintext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        out.ciphertext = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        out.accepted = sqlite3_column_int(stmt, 6);
        out.timestamp = sqlite3_column_int64(stmt, 7);
        out.edited_at = sqlite3_column_int64(stmt, 8);
        out.edit_accepted = sqlite3_column_int(stmt, 9);
        out.delete_accepted = sqlite3_column_int(stmt, 10);
        out.deleted_at = sqlite3_column_int64(stmt, 11);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

void db_manager::upsert_group_update(const group_update &u) {
    const char *sql =
        "INSERT INTO group_updates (group_id, member_id, snapshot_json, version, accepted, created_at) "
        "VALUES (?, ?, ?, ?, 0, unixepoch()) "
        "ON CONFLICT(group_id, member_id) DO UPDATE SET "
        "  snapshot_json = excluded.snapshot_json,"
        "  version = excluded.version,"
        "  accepted = 0;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare group update upsert: " << sqlite3_errmsg(db_.get()) << std::endl;
        return;
    }
    sqlite3_bind_text(stmt, 1, u.group_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, u.member_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, u.snapshot_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, u.version);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "[SQL] Group update upsert failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }
    sqlite3_finalize(stmt);
}

std::vector<db_manager::group_update> db_manager::get_pending_group_updates() {
    std::vector<group_update> result;
    const char *sql = "SELECT group_id, member_id, snapshot_json, version, accepted, created_at "
                    "FROM group_updates WHERE accepted = 0 ORDER BY created_at;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare pending group updates: " << sqlite3_errmsg(db_.get()) << std::endl;
        return result;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        group_update u;
        u.group_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        u.member_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        u.snapshot_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        u.version = sqlite3_column_int64(stmt, 3);
        u.accepted = sqlite3_column_int(stmt, 4);
        u.created_at = sqlite3_column_int64(stmt, 5);
        result.push_back(std::move(u));
    }
    sqlite3_finalize(stmt);
    return result;
}

void db_manager::mark_group_update_accepted(const std::string &group_id, const std::string &member_id) {
    const char *sql = "UPDATE group_updates SET accepted = 1 WHERE group_id = ? AND member_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[SQL] Failed to prepare group update accept: " << sqlite3_errmsg(db_.get()) << std::endl;
        return;
    }
    sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, member_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "[SQL] Group update accept failed: " << sqlite3_errmsg(db_.get()) << std::endl;
    }
    sqlite3_finalize(stmt);
}
