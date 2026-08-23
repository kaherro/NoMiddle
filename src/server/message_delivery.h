#pragma once
#include "../db/db_manager.h"
#include <string>

std::string generate_uuid();

std::string deliver_message(db_manager &db, int64_t sender_id, int64_t recipient_id, 
    const std::string &text, int64_t timestamp);