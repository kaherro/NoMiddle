#pragma once
#include "../db/db_manager.h"
#include <string>
#include <optional>

std::string generate_uuid();

std::optional<std::string> deliver_message(db_manager &db, int64_t sender_id, int64_t recipient_id, 
    const std::string &text, int64_t timestamp);

bool retry_deliver_message(db_manager &db, const db_manager::message &msg);