#pragma once
#include <string>
#include <optional>

struct KeyPair {
    std::string public_key_b64;
    std::string private_key_b64; 
};

bool generate_and_save_keypair(const std::string &private_key_path, KeyPair &out_keys);
std::optional<KeyPair> load_or_create_keypair(const std::string &private_key_path);

std::string bin_to_base64(const unsigned char* data, size_t len);
bool base64_to_bin(const std::string &b64, unsigned char* out, size_t out_len);