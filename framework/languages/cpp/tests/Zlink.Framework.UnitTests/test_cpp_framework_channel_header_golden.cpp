/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/messaging/envelope_codec.hpp"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{
using json = nlohmann::json;
using zlink::framework::runtime::messaging::envelope_codec_t;
using zlink::framework::runtime::messaging::envelope_header_t;
using zlink::framework::runtime::messaging::message_kind_t;

std::string from_hex (const std::string &hex)
{
    std::string bytes;
    bytes.reserve (hex.size () / 2);
    for (std::size_t i = 0; i < hex.size (); i += 2)
        bytes.push_back (static_cast<char> (std::stoul (hex.substr (i, 2), nullptr, 16)));
    return bytes;
}

envelope_header_t expected_header (const json &value)
{
    envelope_header_t header;
    header.kind = static_cast<message_kind_t> (value.at ("kind").get<int> ());
    header.channel_name = value.at ("channel_name").get<std::string> ();
    header.message_name = value.at ("message_name").get<std::string> ();
    header.content_type = value.at ("content_type").get<std::string> ();
    if (value.contains ("correlation_id"))
        header.correlation_id = value.at ("correlation_id").get<std::string> ();
    if (value.contains ("deadline"))
        header.deadline = value.at ("deadline").get<std::string> ();
    for (const auto *name : {"error_code", "error_message", "flow_id", "source", "topic"})
        if (value.contains (name))
            value.at (name).get_to (*([&] () -> std::optional<std::string> * {
                if (std::string (name) == "error_code")
                    return &header.error_code;
                if (std::string (name) == "error_message")
                    return &header.error_message;
                if (std::string (name) == "flow_id")
                    return &header.flow_id;
                if (std::string (name) == "source")
                    return &header.source;
                return &header.topic;
            }) ());
    if (value.contains ("flow_origin"))
        header.flow_origin =
          static_cast<zlink::framework::flow_origin_t> (value.at ("flow_origin").get<int> ());
    if (value.contains ("metadata"))
        for (const auto &entry : value.at ("metadata"))
            header.metadata.emplace (entry.at (0).get<std::string> (),
                                     entry.at (1).get<std::string> ());
    return header;
}

bool same_header (const envelope_header_t &left, const envelope_header_t &right)
{
    return left.kind == right.kind && left.channel_name == right.channel_name
           && left.message_name == right.message_name && left.content_type == right.content_type
           && left.correlation_id == right.correlation_id && left.deadline == right.deadline
           && left.error_code == right.error_code && left.error_message == right.error_message
           && left.flow_id == right.flow_id && left.flow_origin == right.flow_origin
           && left.source == right.source && left.topic == right.topic
           && left.metadata == right.metadata;
}

std::vector<bool> tracing_modes (const json &fixture)
{
    if (!fixture.contains ("tracing"))
        return {true, false};
    std::vector<bool> modes;
    for (const auto &mode : fixture.at ("tracing"))
        modes.push_back (mode.get<std::string> () == "on");
    return modes;
}

} // namespace

int main ()
{
    std::ifstream input (ZLINK_CHANNEL_HEADER_GOLDEN_PATH);
    assert (input.good ());
    const auto golden = json::parse (input);
    envelope_codec_t codec;
    bool all_passed = true;
    for (const auto &fixture : golden.at ("valid")) {
        const auto name = fixture.at ("name").get<std::string> ();
        const auto bytes = from_hex (fixture.at ("hex").get<std::string> ());
        for (const bool capture_flow : tracing_modes (fixture)) {
            const auto wire_header = expected_header (fixture.at ("header"));
            auto expected = wire_header;
            if (!capture_flow) {
                expected.flow_id.reset ();
                expected.flow_origin.reset ();
            }
            const auto decoded = codec.decode_header (
              std::span<const std::byte> (reinterpret_cast<const std::byte *> (bytes.data ()),
                                          bytes.size ()),
              capture_flow);
            if (!decoded || !same_header (decoded.value (), expected)) {
                std::cerr << "valid fixture failed: " << name
                          << " tracing=" << (capture_flow ? "on" : "off") << " decode\n";
                all_passed = false;
                continue;
            }
            const auto message = zlink::message_t::from (bytes);
            const auto viewed = codec.decode_header_view (message, capture_flow);
            if (!viewed || !same_header (viewed.value (), expected)) {
                std::cerr << "valid fixture failed: " << name
                          << " tracing=" << (capture_flow ? "on" : "off") << " view decode\n";
                all_passed = false;
                continue;
            }
            const auto input_begin = reinterpret_cast<std::uintptr_t> (message.data ());
            const auto input_end = input_begin + message.size ();
            const auto check_borrow = [&] (const auto &text) {
                if (text.empty ())
                    return true;
                const auto begin = reinterpret_cast<std::uintptr_t> (text.data ());
                return begin >= input_begin && begin + text.size () <= input_end;
            };
            if (!check_borrow (viewed.value ().channel_name)
                || !check_borrow (viewed.value ().message_name)
                || !check_borrow (viewed.value ().content_type)
                || (!viewed.value ().correlation_id.empty ()
                    && !check_borrow (viewed.value ().correlation_id))
                || (viewed.value ().deadline && !check_borrow (*viewed.value ().deadline))) {
                std::cerr << "valid fixture failed: " << name
                          << " view did not borrow retained envelope header\n";
                all_passed = false;
            }
            if (fixture.value ("encode", true)) {
                try {
                    const auto encoded_message = codec.encode_header (wire_header);
                    const auto encoded = encoded_message.bytes ();
                    const std::string encoded_bytes (
                      reinterpret_cast<const char *> (encoded.data ()), encoded.size ());
                    if (encoded_bytes == bytes)
                        continue;
                }
                catch (const zlink::framework::framework_exception_t &error) {
                    std::cerr << "valid fixture encode rejected: " << name
                              << " tracing=" << (capture_flow ? "on" : "off")
                              << " error=" << error.what () << "\n";
                }
                {
                    std::cerr << "valid fixture failed: " << name
                              << " tracing=" << (capture_flow ? "on" : "off") << " encode\n";
                    all_passed = false;
                }
            }
        }
    }
    for (const auto &fixture : golden.at ("reject")) {
        const auto name = fixture.at ("name").get<std::string> ();
        const auto bytes = from_hex (fixture.at ("hex").get<std::string> ());
        for (const bool capture_flow : tracing_modes (fixture)) {
            std::optional<envelope_header_t> rejected_owned;
            const auto decoded = codec.decode_header (
              std::span<const std::byte> (reinterpret_cast<const std::byte *> (bytes.data ()),
                                          bytes.size ()),
              capture_flow, &rejected_owned);
            if (decoded
                || decoded.error_kind ()
                     != zlink::framework::framework_error_kind_t::protocol_error) {
                std::cerr << "reject fixture failed: " << name
                          << " tracing=" << (capture_flow ? "on" : "off") << "\n";
                all_passed = false;
            }
            std::optional<envelope_header_t> rejected_view;
            const auto viewed = codec.decode_header_view (zlink::message_t::from (bytes),
                                                          capture_flow, &rejected_view);
            if (viewed || viewed.error_kind () != decoded.error_kind ()
                || std::string_view (viewed.error ()->what ())
                     != std::string_view (decoded.error ()->what ())
                || rejected_owned.has_value () != rejected_view.has_value ()
                || (rejected_owned && !same_header (*rejected_owned, *rejected_view))) {
                std::cerr << "reject fixture failed: " << name
                          << " view/owned rejection differs tracing="
                          << (capture_flow ? "on" : "off") << "\n";
                all_passed = false;
            }
        }
    }
    assert (all_passed);
}
