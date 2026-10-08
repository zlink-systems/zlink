#pragma once
#include "runtime/locations/actor_authority_payload.hpp"
#include <cassert>
#include <string_view>
namespace zlink::framework::tests
{
using namespace zlink::framework::runtime;
inline std::vector<std::byte> from_hex (std::string_view value)
{
    const auto digit = [] (char ch) -> std::uint8_t {
        assert ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'));
        return static_cast<std::uint8_t> (ch <= '9' ? ch - '0' : ch - 'a' + 10);
    };
    assert (value.size () % 2 == 0);
    std::vector<std::byte> result;
    result.reserve (value.size () / 2);
    for (std::size_t index = 0; index < value.size (); index += 2)
        result.push_back (
          static_cast<std::byte> ((digit (value[index]) << 4) | digit (value[index + 1])));
    return result;
}

inline std::vector<std::byte> with_relocation_slot (const std::vector<std::byte> &authority,
                                                    const std::vector<std::byte> &slot)
{
    const auto canonical = decode_canonical_authority_payload (authority);
    assert (canonical && canonical->body.size () >= 10);
    std::vector<std::byte> body (canonical->body.begin (), canonical->body.end () - 10);
    actor_authority_detail::append_u8 (body, 1);
    actor_authority_detail::append_u32be (body, static_cast<std::uint32_t> (slot.size ()));
    actor_authority_detail::append_bytes (body, slot);
    actor_authority_detail::append_u8 (body, 0); // activation recovery absent
    actor_authority_detail::append_u32be (body, 0);
    return encode_canonical_authority_payload ({std::move (body)});
}
}
