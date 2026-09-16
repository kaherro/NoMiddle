#include "auth.h"
#include <sodium.h>
#include <vector>

std::string b64url_encode(const unsigned char* data, size_t len) {
    size_t out_len = sodium_base64_encoded_len(len, sodium_base64_VARIANT_URLSAFE);
    std::string out(out_len, '\0');
    sodium_bin2base64(out.data(), out_len, data, len, sodium_base64_VARIANT_URLSAFE);
    out.resize(out_len - 1);
    return out;
}

std::string random_b64url(int nbytes) {
    std::vector<unsigned char> buf(nbytes);
    randombytes_buf(buf.data(), buf.size());
    return b64url_encode(buf.data(), buf.size());
}

std::string hash_secret(const std::string &secret, const std::string &salt) {
    unsigned char out[crypto_generichash_BYTES];
    crypto_generichash(out, sizeof(out),
        reinterpret_cast<const unsigned char*>(secret.data()), secret.size(),
        reinterpret_cast<const unsigned char*>(salt.data()), salt.size());
    return b64url_encode(out, sizeof(out));
}

std::string header_value(const crow::request &req, const std::string &name) {
    return req.get_header_value(name);
}

bool authenticate(db_manager &db, const std::string &client_id, const std::string &secret) {
    if (client_id.empty() || secret.empty()) return false;
    db_manager::client info;
    if (!db.get_client(client_id, info)) return false;
    return hash_secret(secret, info.salt) == info.secret_hash;
}