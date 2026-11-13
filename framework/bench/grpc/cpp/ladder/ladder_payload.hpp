/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "../common/bench_common.hpp"
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/json.hpp>
#include <cstddef>
#include <tuple>
#include <vector>

namespace zlink_cpp_bench::ladder
{
// Ordinary DTO metadata selects the default Framework JSON serializer.
struct typed_payload_t
{
    static constexpr const char *packet_name = "BenchPayload";
    std::vector<std::byte> bytes;
    const auto &body () const noexcept { return bytes; }
    void set_body (const std::vector<std::byte> &value) { bytes = value; }
};

inline typed_payload_t make_typed_payload (size_t size, uint32_t run, phase_t phase, uint64_t seq)
{
    typed_payload_t value;
    value.bytes.assign (size, std::byte{0xab});
    stamp_payload (value.bytes.data (), size, run, phase, seq);
    return value;
}
}

template <> struct zlink::json::dto_traits<zlink_cpp_bench::ladder::typed_payload_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("body", &zlink_cpp_bench::ladder::typed_payload_t::bytes)};
};

namespace zlink_cpp_bench::ladder
{
inline auto typed_serializer ()
{
    static const zlink::framework::serializer_registry_t registry;
    return registry.get<typed_payload_t> ();
}
}
