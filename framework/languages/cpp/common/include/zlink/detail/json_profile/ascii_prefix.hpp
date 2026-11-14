#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
namespace zlink::detail::json_profile {
inline std::size_t plain_ascii_prefix(std::uint64_t word) {
 constexpr std::uint64_t high=0x8080808080808080ULL, ones=0x0101010101010101ULL, spaces=0x2020202020202020ULL;
 const auto quotes=word^0x2222222222222222ULL, slashes=word^0x5c5c5c5c5c5c5c5cULL;
 const auto special=(word&high)|((word-spaces)&~word&high)|((quotes-ones)&~quotes&high)|((slashes-ones)&~slashes&high);
 const auto leading=std::endian::native==std::endian::little ? std::countr_zero(special) : std::countl_zero(special);
 return static_cast<std::size_t>(leading/8);
}
}
