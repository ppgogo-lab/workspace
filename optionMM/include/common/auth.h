#pragma once

#include <string>
#include <string_view>

namespace omm {

[[nodiscard]] bool password_hash_encoded(std::string_view encoded) noexcept;
[[nodiscard]] std::string hash_password(std::string_view password);
[[nodiscard]] bool verify_password(std::string_view password,
                                   std::string_view encoded_hash) noexcept;
[[nodiscard]] std::string generate_session_token();

} // namespace omm
