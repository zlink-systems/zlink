/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace zlink::stream_connector::detail
{

inline constexpr char admission_enqueue_name[] = "$zlink.admission.enqueue";
inline constexpr char admission_position_name[] = "$zlink.admission.position";
inline constexpr char admission_ticket_name[] = "$zlink.admission.ticket";
inline constexpr char admission_required_key[] = "$zlink.admission.queueRequired";

enum class admission_update_kind_t
{
    either,
    position,
    ticket
};

inline std::optional<admission_update_kind_t> admission_push_kind (std::string_view name)
{
    if (name == admission_position_name) {
        return admission_update_kind_t::position;
    }
    if (name == admission_ticket_name) {
        return admission_update_kind_t::ticket;
    }
    return std::nullopt;
}

struct admission_position_t
{
    std::int32_t position;
    std::chrono::milliseconds estimated_wait;
};

using admission_update_t = std::variant<admission_position_t, std::string>;

std::optional<admission_update_t> decode_admission_update (const std::vector<std::uint8_t> &payload,
                                                           admission_update_kind_t kind);

} // namespace zlink::stream_connector::detail
