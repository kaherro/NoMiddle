#pragma once
#include "../db/db_manager.h"
#include <string>
#include <optional>

std::string generate_uuid();

std::optional<std::string> deliver_message(db_manager &db, const std::string &sender_id, const std::string &recipient_id,
    const std::string &ciphertext, const std::string &plaintext, int64_t timestamp); 

bool deliver_message_edit(db_manager &db, const std::string &message_id, const std::string &sender_id,
    const std::string &recipient_id, const std::string &ciphertext, int64_t edited_at);

bool retry_deliver_message(db_manager &db, const db_manager::message &msg);

bool retry_message_edit(db_manager &db, const db_manager::message &msg);

bool deliver_message_delete(db_manager &db, const std::string &message_id,
    const std::string &sender_id, const std::string &recipient_id);

bool retry_message_delete(db_manager &db, const db_manager::message &msg);

bool deliver_group_update(db_manager &db, const std::string &group_id,
    const std::string &sender_id, const std::vector<db_manager::group_member> &members, int64_t version);

bool retry_group_update(db_manager &db, const db_manager::group_update &u);