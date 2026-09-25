#include "message_crypto.h"
#include "key_manager.h"
#include <sodium.h>
#include <vector>
#include <algorithm>
#include <iostream>

std::optional<std::string> encrypt_message(const std::string &plaintext,
                                            const std::string &recipient_public_key_b64,
                                            const std::string &sender_private_key_b64) {
    if (sodium_init() < 0) {
        std::cerr << "[CRYPTO] Failed to initialize libsodium\n";
        return std::nullopt;
    }

    unsigned char recipient_pk[crypto_box_PUBLICKEYBYTES];
    unsigned char sender_sk[crypto_box_SECRETKEYBYTES];
    if (!base64_to_bin(recipient_public_key_b64, recipient_pk, sizeof(recipient_pk))) {
        std::cerr << "[CRYPTO] Invalid recipient public key\n";
        return std::nullopt;
    }
    if (!base64_to_bin(sender_private_key_b64, sender_sk, sizeof(sender_sk))) {
        std::cerr << "[CRYPTO] Invalid sender private key\n";
        return std::nullopt;
    }

    unsigned char nonce[crypto_box_NONCEBYTES];
    randombytes_buf(nonce, sizeof(nonce));
    std::vector<unsigned char> ciphertext(plaintext.size() + crypto_box_MACBYTES);
    int rc = crypto_box_easy(
        ciphertext.data(),
        reinterpret_cast<const unsigned char*>(plaintext.data()), plaintext.size(),
        nonce, recipient_pk, sender_sk
    );
    sodium_memzero(sender_sk, sizeof(sender_sk));
    if (rc != 0) {
        std::cerr << "[CRYPTO] Encryption failed\n";
        return std::nullopt;
    }
    std::vector<unsigned char> blob(sizeof(nonce) + ciphertext.size());
    std::copy(nonce, nonce + sizeof(nonce), blob.begin());
    std::copy(ciphertext.begin(), ciphertext.end(), blob.begin() + sizeof(nonce));
    return bin_to_base64(blob.data(), blob.size());
}

std::optional<std::string> decrypt_message(const std::string &ciphertext_b64,
                                            const std::string &sender_public_key_b64,
                                            const std::string &recipient_private_key_b64) {
    if (sodium_init() < 0) {
        std::cerr << "[CRYPTO] Failed to initialize libsodium\n";
        return std::nullopt;
    }

    unsigned char sender_pk[crypto_box_PUBLICKEYBYTES];
    unsigned char recipient_sk[crypto_box_SECRETKEYBYTES];
    if (!base64_to_bin(sender_public_key_b64, sender_pk, sizeof(sender_pk))) {
        std::cerr << "[CRYPTO] Invalid sender public key\n";
        return std::nullopt;
    }
    if (!base64_to_bin(recipient_private_key_b64, recipient_sk, sizeof(recipient_sk))) {
        std::cerr << "[CRYPTO] Invalid recipient private key\n";
        return std::nullopt;
    }

    std::vector<unsigned char> blob(ciphertext_b64.size());
    size_t blob_len = 0;
    int decode_rc = sodium_base642bin(
        blob.data(), blob.size(),
        ciphertext_b64.c_str(), ciphertext_b64.size(),
        nullptr, &blob_len, nullptr,
        sodium_base64_VARIANT_ORIGINAL
    );
    if (decode_rc != 0) {
        std::cerr << "[CRYPTO] Failed to decode base64 ciphertext\n";
        sodium_memzero(recipient_sk, sizeof(recipient_sk));
        return std::nullopt;
    }
    blob.resize(blob_len);

    if (blob.size() < crypto_box_NONCEBYTES + crypto_box_MACBYTES) {
        std::cerr << "[CRYPTO] Ciphertext too short — corrupted?\n";
        sodium_memzero(recipient_sk, sizeof(recipient_sk));
        return std::nullopt;
    }

    const unsigned char* nonce = blob.data();
    const unsigned char* actual_ciphertext = blob.data() + crypto_box_NONCEBYTES;
    size_t actual_ciphertext_len = blob.size() - crypto_box_NONCEBYTES;
    std::vector<unsigned char> decrypted(actual_ciphertext_len - crypto_box_MACBYTES);
    int rc = crypto_box_open_easy(
        decrypted.data(),
        actual_ciphertext, actual_ciphertext_len,
        nonce, sender_pk, recipient_sk
    );
    sodium_memzero(recipient_sk, sizeof(recipient_sk));
    if (rc != 0) {
        std::cerr << "[CRYPTO] Decryption failed - tampered message or wrong keys\n";
        return std::nullopt;
    }
    return std::string(decrypted.begin(), decrypted.end());
}