/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../common/game_message.hpp"
#include <zlink/framework.hpp>

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fw = zlink::framework;

void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}

int main ()
{
    std::ifstream input (BENCH_GAME_JSON_FIXTURE);
    require (static_cast<bool> (input), "cannot open shared game JSON fixture");
    const auto fixture = nlohmann::json::parse (input);
    const fw::serializer_registry_t registry;
    const auto serializer = registry.get<zlink_cpp_bench::game_message_t> ();
    constexpr uint64_t sent_ns = 1700000000000000000ULL;

    for (const auto payload_size : {1024U, 4096U}) {
        const auto &golden = fixture.at ("golden").at (std::to_string (payload_size));
        const auto expected = golden.at ("json").get<std::string> ();
        require (expected.size () == golden.at ("byteLength").get<size_t> (),
                 "fixture golden JSON byte length mismatch");
        const auto value = zlink_cpp_bench::make_game_message (
          payload_size, 123, zlink_cpp_bench::phase_active, 456, sent_ns);
        require (value.items.size () == golden.at ("itemCount").get<size_t> (),
                 "fixture item count differs from the selected payload size");
        const auto raw_encoded = zlink_cpp_bench::encode_game_json (value);
        require (raw_encoded == expected, "nlohmann game JSON differs from golden bytes");
        const auto framework_encoded = serializer.serialize (value).to_string ();
        require (framework_encoded == expected, "framework typed JSON differs from golden bytes");

        const auto raw_decoded =
          zlink_cpp_bench::decode_game_json (expected.data (), expected.size ());
        require (zlink_cpp_bench::game_stamp_matches (
                   raw_decoded, 123, zlink_cpp_bench::phase_active, 456, payload_size),
                 "raw JSON decoder lost the stamp fields");
        const auto framework_decoded =
          serializer.deserialize (fw::detail::encoded_payload_from_raw (std::span<const std::byte> (
            reinterpret_cast<const std::byte *> (expected.data ()), expected.size ())));
        require (zlink_cpp_bench::game_stamp_matches (
                   framework_decoded, 123, zlink_cpp_bench::phase_active, 456, payload_size),
                 "framework typed JSON decoder lost the stamp fields");
    }
    std::cout << "cpp game JSON fixture 1024/4096 encode and decode PASS\n";
}
