#pragma once
#include <string>
#include <crow.h>
#include "../db/db_manager.h"

std::string random_b64url(int nbytes);
std::string hash_secret(const std::string &secret, const std::string &salt);
std::string header_value(const crow::request &req, const std::string &name);
bool authenticate(db_manager &db, const std::string &client_id, const std::string &secret);