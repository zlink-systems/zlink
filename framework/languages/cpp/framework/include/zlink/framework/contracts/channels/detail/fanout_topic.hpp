/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/errors/error.hpp>

#include <string_view>

namespace zlink::framework::detail
{

inline constexpr std::string_view fanout_liveness_topic{"\x01\x5a\x4c\x46\x31", 5};

inline bool is_reserved_fanout_topic (std::string_view topic) noexcept
{
    return topic.starts_with (fanout_liveness_topic);
}

inline void require_public_fanout_topic (std::string_view topic)
{
    if (is_reserved_fanout_topic (topic)) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "fanout topic is reserved for framework liveness");
    }
}

} // namespace zlink::framework::detail
