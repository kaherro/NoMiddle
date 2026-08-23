#pragma once

#include <string>
#include <optional>

std::optional<int64_t> send_message(const std::string& url, const std::string& json_body);