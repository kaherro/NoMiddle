#include "key_manager.h"
#include <sodium.h>
#include <fstream>
#include <iostream>
#include <sys/stat.h>

std::string bin_to_base64(const unsigned char* data, size_t len) {
    size_t b64_len = sodium_base64_encoded_len(len, sodium_base64_VARIANT_ORIGINAL);
    std::string out(b64_len, '\0');
    sodium_bin2base64(out.data(), b64_len, data, len, sodium_base64_VARIANT_ORIGINAL);
    out.resize(b64_len - 1); // delete '\0'
    return out;
}

bool base64_to_bin(const std::string &b64, unsigned char* out, size_t out_len) {
    size_t decoded_len = 0;
    int rc = sodium_base642bin(
        out, out_len,
        b64.c_str(), b64.size(),
        nullptr, &decoded_len, nullptr,
        sodium_base64_VARIANT_ORIGINAL
    );
    return rc == 0 && decoded_len == out_len;
}

bool generate_and_save_keypair(const std::string &private_key_path, KeyPair &out_keys) {
    if(sodium_init() < 0) {
        std::cerr << "[KEYS] Failed to initialize libsodium\n";
        return false;
    }

    unsigned char pk[crypto_box_PUBLICKEYBYTES];
    unsigned char sk[crypto_box_SECRETKEYBYTES];
    crypto_box_keypair(pk, sk);
    std::ofstream file(private_key_path, std::ios::binary);
    if(!file) {
        std::cerr << "[KEYS] Failed to open " << private_key_path << " for writing\n";
        sodium_memzero(sk, sizeof(sk));
        return false;
    }
    file.write(reinterpret_cast<const char*>(sk), sizeof(sk));
    if(!file.good()) {
        std::cerr << "[KEYS] Error while writing private key in " << private_key_path << "\n";
    }
    file.close();

    chmod(private_key_path.c_str(), S_IRUSR | S_IWUSR);
    out_keys.public_key_b64  = bin_to_base64(pk, sizeof(pk));
    out_keys.private_key_b64 = bin_to_base64(sk, sizeof(sk));
    sodium_memzero(sk, sizeof(sk));
    return true;
}

std::optional<KeyPair> load_or_create_keypair(const std::string &private_key_path) {
    if(sodium_init() < 0) {
        std::cerr << "[KEYS] Failed to initialize libsodium\n";
        return std::nullopt;
    }

    std::ifstream file(private_key_path, std::ios::binary);
    if(!file) {
        std::cout << "[KEYS] No private key found in " << private_key_path << ", generating new one\n";
        KeyPair keys;
        if (!generate_and_save_keypair(private_key_path, keys)) return std::nullopt;
        return keys;
    }

    unsigned char sk[crypto_box_SECRETKEYBYTES];
    file.read(reinterpret_cast<char*>(sk), sizeof(sk));
    if (!file || file.gcount() != sizeof(sk)) {
        std::cerr << "[KEYS] Private key file is corrupted or wrong size\n";
        sodium_memzero(sk, sizeof(sk));
        return std::nullopt;
    }

    unsigned char pk[crypto_box_PUBLICKEYBYTES];
    crypto_scalarmult_base(pk, sk);
    KeyPair keys;
    keys.public_key_b64  = bin_to_base64(pk, sizeof(pk));
    keys.private_key_b64 = bin_to_base64(sk, sizeof(sk));
    sodium_memzero(sk, sizeof(sk));
    return keys;
}