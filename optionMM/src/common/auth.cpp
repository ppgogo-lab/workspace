#include "common/auth.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace omm {

namespace {

constexpr char kPrefix[] = "pbkdf2_sha256";
constexpr int kIterations = 200000;
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kDigestBytes = 32;

[[nodiscard]] char hex_digit(uint8_t value) noexcept {
    return static_cast<char>(value < 10 ? ('0' + value) : ('a' + (value - 10)));
}

[[nodiscard]] int from_hex(char ch) noexcept {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
    if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
    return -1;
}

[[nodiscard]] std::string hex_encode(const uint8_t* data, std::size_t size) {
    std::string out;
    out.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out[2 * i] = hex_digit(static_cast<uint8_t>((data[i] >> 4) & 0x0F));
        out[2 * i + 1] = hex_digit(static_cast<uint8_t>(data[i] & 0x0F));
    }
    return out;
}

[[nodiscard]] bool hex_decode(std::string_view text, std::vector<uint8_t>* out) noexcept {
    if (out == nullptr || (text.size() & 1u) != 0) return false;
    out->clear();
    out->reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = from_hex(text[i]);
        const int lo = from_hex(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

[[nodiscard]] bool derive_pbkdf2(std::string_view password,
                                 const uint8_t* salt,
                                 std::size_t salt_size,
                                 int iterations,
                                 uint8_t* out_digest,
                                 std::size_t out_size) noexcept {
    return PKCS5_PBKDF2_HMAC(password.data(),
                            static_cast<int>(password.size()),
                            salt,
                            static_cast<int>(salt_size),
                            iterations,
                            EVP_sha256(),
                            static_cast<int>(out_size),
                            out_digest) == 1;
}

} // namespace

bool password_hash_encoded(std::string_view encoded) noexcept {
    return encoded.rfind(kPrefix, 0) == 0;
}

std::string hash_password(std::string_view password) {
    std::array<uint8_t, kSaltBytes> salt{};
    std::array<uint8_t, kDigestBytes> digest{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
        throw std::runtime_error("auth: RAND_bytes failed");
    }
    if (!derive_pbkdf2(password, salt.data(), salt.size(), kIterations,
                       digest.data(), digest.size())) {
        throw std::runtime_error("auth: PKCS5_PBKDF2_HMAC failed");
    }

    std::ostringstream os;
    os << kPrefix << '$' << kIterations << '$'
       << hex_encode(salt.data(), salt.size()) << '$'
       << hex_encode(digest.data(), digest.size());
    return os.str();
}

bool verify_password(std::string_view password,
                     std::string_view encoded_hash) noexcept {
    if (!password_hash_encoded(encoded_hash)) return false;

    const std::size_t first_sep = encoded_hash.find('$');
    if (first_sep == std::string_view::npos) return false;
    const std::size_t second_sep = encoded_hash.find('$', first_sep + 1);
    if (second_sep == std::string_view::npos) return false;
    const std::size_t third_sep = encoded_hash.find('$', second_sep + 1);
    if (third_sep == std::string_view::npos) return false;

    int iterations = 0;
    try {
        iterations = std::stoi(std::string(encoded_hash.substr(first_sep + 1,
                                                               second_sep - first_sep - 1)));
    } catch (...) {
        return false;
    }
    if (iterations <= 0) return false;

    std::vector<uint8_t> salt;
    std::vector<uint8_t> expected;
    if (!hex_decode(encoded_hash.substr(second_sep + 1, third_sep - second_sep - 1), &salt)
        || !hex_decode(encoded_hash.substr(third_sep + 1), &expected)
        || expected.empty()) {
        return false;
    }

    std::vector<uint8_t> actual(expected.size(), 0);
    if (!derive_pbkdf2(password, salt.data(), salt.size(), iterations,
                       actual.data(), actual.size())) {
        return false;
    }
    return CRYPTO_memcmp(actual.data(), expected.data(), expected.size()) == 0;
}

std::string generate_session_token() {
    std::array<uint8_t, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("auth: RAND_bytes failed");
    }
    return hex_encode(bytes.data(), bytes.size());
}

} // namespace omm
