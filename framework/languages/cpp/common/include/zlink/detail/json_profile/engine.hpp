/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::detail::json_profile
{

enum class token_kind : std::uint8_t
{
    null,
    boolean,
    integer,
    unsigned_integer,
    floating,
    raw_number,
    raw_json,
    string,
    object,
    array
};

struct token
{
    token_kind kind = token_kind::null;
    bool boolean = false;
    std::int64_t integer = 0;
    std::uint64_t unsigned_integer = 0;
    double floating = 0;
    std::size_t text_offset = 0;
    std::size_t text_length = 0;
    std::size_t key_offset = 0;
    std::size_t key_length = 0;
    std::size_t child_count = 0;
};

struct token_buffer
{
    std::vector<token> tokens;
    std::string strings;
};

inline thread_local token_buffer reusable_tokens;

// Framework and Stream Connector each compile the engine under their own
// namespace so a consumer of both links two distinct symbols. A target exports
// ZLINK_JSON_PROFILE_ENGINE_FRAMEWORK or _CONNECTOR; the Framework name wins when
// both arrive, and ZLINK_JSON_PROFILE_ENGINE_NS set first (by the engine's own
// library) is kept.
#ifndef ZLINK_JSON_PROFILE_ENGINE_NS
#if defined(ZLINK_JSON_PROFILE_ENGINE_CONNECTOR) && !defined(ZLINK_JSON_PROFILE_ENGINE_FRAMEWORK)
#define ZLINK_JSON_PROFILE_ENGINE_NS connector_engine
#else
#define ZLINK_JSON_PROFILE_ENGINE_NS framework_engine
#endif
#endif

namespace ZLINK_JSON_PROFILE_ENGINE_NS
{
bool parse_tokens (std::string_view input, token_buffer &output);
}
using ZLINK_JSON_PROFILE_ENGINE_NS::parse_tokens;

} // namespace zlink::detail::json_profile
