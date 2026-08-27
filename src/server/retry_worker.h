#pragma once
#include "../db/db_manager.h"
#include <atomic>

void start_retrying_worker(db_manager &db, std::atomic<bool> &running);