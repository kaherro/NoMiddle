#include "http_client.h"
#include <cpr/cpr.h>

std::optional<int64_t> send_message(const std::string& url, const std::string& json_body) {
    cpr::Response r = cpr::Post(
        cpr::Url{url},
        cpr::Body{json_body},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Timeout{10000}
    );

    if (r.error.code != cpr::ErrorCode::OK) {
        return std::nullopt;
    }

    return r.status_code;
}