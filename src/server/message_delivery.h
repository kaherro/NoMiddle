#pragma once
#include "../db/db_manager.h"
#include <string>
#include <optional>

std::string generate_uuid();

std::optional<std::string> deliver_message(db_manager &db, const std::string &sender_id, const std::string &recipient_id,
    const std::string &text, int64_t timestamp);

bool retry_deliver_message(db_manager &db, const db_manager::message &msg);