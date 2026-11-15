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
        require (raw_encoded == expected, "binding game JSON differs from golden bytes");
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
        require (zlink_cpp_bench::encode_game_json (raw_decoded) == expected,
                 "binding JSON decoder changed body fields");
        require (serializer.serialize (framework_decoded).to_string () == expected,
                 "framework JSON decoder changed body fields");
    }
    for (const auto payload_size : {1024U, 4096U}) {
        const auto valid = nlohmann::json::parse (
          fixture.at ("golden").at (std::to_string (payload_size)).at ("json").get<std::string> ());
        std::vector<nlohmann::json> invalid;
        for (const auto &[name, ignored] : valid.items ()) {
            auto missing = valid;
            missing.erase (name);
            invalid.push_back (missing);
            auto null_field = valid;
            null_field[name] = nullptr;
            invalid.push_back (null_field);
        }
        for (const auto &[name, ignored] : valid.at ("items").at (0).items ()) {
            auto missing = valid;
            missing["items"][0].erase (name);
            invalid.push_back (missing);
            auto null_field = valid;
            null_field["items"][0][name] = nullptr;
            invalid.push_back (null_field);
        }
        for (const auto &bad_gold : {nlohmann::json ("01"), nlohmann::json (1)}) {
            auto bad = valid;
            bad["gold"] = bad_gold;
            invalid.push_back (bad);
        }
        auto bad_heading = valid;
        bad_heading["heading"] = "1";
        invalid.push_back (bad_heading);
        for (const auto &bad : invalid) {
            const auto text = bad.dump ();
            bool raw_rejected = false;
            bool framework_rejected = false;
            std::string raw_error;
            std::string framework_error;
            try {
                (void) zlink_cpp_bench::decode_game_json (text.data (), text.size ());
            }
            catch (const std::invalid_argument &error) {
                raw_error = error.what ();
                raw_rejected = true;
            }
            try {
                (void) serializer.deserialize (
                  fw::detail::encoded_payload_from_raw (std::span<const std::byte> (
                    reinterpret_cast<const std::byte *> (text.data ()), text.size ())));
            }
            catch (const fw::framework_exception_t &error) {
                require (error.kind () == fw::framework_error_kind_t::protocol_error,
                         "framework must classify payload decode failure as protocol_error");
                try {
                    std::rethrow_if_nested (error);
                }
                catch (const std::invalid_argument &cause) {
                    framework_error = cause.what ();
                    framework_rejected = true;
                }
            }
            require (raw_rejected && framework_rejected && raw_error == framework_error,
                     "both JSON codecs must reject with the same invalid_argument cause");
        }
    }
    std::cout << "cpp game JSON fixture 1024/4096 encode and decode PASS\n";
}
