#pragma once
#include <string>
#include <optional>

std::optional<std::string> encrypt_message(const std::string &plaintext,
                                            const std::string &recipient_public_key_b64,
                                            const std::string &sender_private_key_b64);

std::optional<std::string> decrypt_message(const std::string &ciphertext_b64,
                                            const std::string &sender_public_key_b64,
                                            const std::string &recipient_private_key_b64);