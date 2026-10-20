/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace
{
using zlink::framework::tests::count_allocations;

namespace fw = zlink::framework;
namespace msg = fw::runtime::messaging;
constexpr std::string_view json_content_type = "application/json";
struct payload_t
{
    int value = 7;
};

TEST (EnvelopeAllocations, CounterDetectsAnOwningStringCopy)
{
    const std::string original (128, 'x');
    std::optional<std::string> copy;
    EXPECT_EQ (count_allocations ([&] {}), 0u);
    EXPECT_EQ (count_allocations ([&] { copy.emplace (original); }), 1u);
    EXPECT_EQ (*copy, original);
    void *probe = nullptr;
    constexpr auto json_allocation_size = json_content_type.size () + 1;
    EXPECT_EQ (count_allocations ([&] { probe = ::operator new (json_allocation_size); },
                                  json_allocation_size),
               1u);
    ::operator delete (probe);
    EXPECT_EQ (count_allocations ([&] { probe = ::operator new (64); }, json_allocation_size), 0u);
    ::operator delete (probe);
}

TEST (EnvelopeAllocations, TypedAndErasedEncodingDoNotCloneTheHeader)
{
    fw::serializer_registry_t serializers;
    const std::string content_type = "application/" + std::string (128, 'p');
    serializers.add<payload_t> (
      [] (const payload_t &) { return fw::encoded_payload_t::from_string ("7"); },
      [] (const fw::encoded_payload_t &) { return payload_t{}; }, content_type);
    msg::envelope_codec_t codec;
    msg::envelope_header_t header;
    header.channel_name = std::string (128, 'c');
    header.message_name = std::string (128, 'm');
    header.content_type = "application/original";
    header.correlation_id = std::string (32, 'a');
    header.topic = std::string (128, 't');
    header.source = std::string (128, 's');
    header.metadata.emplace (std::string (128, 'k'), std::string (128, 'v'));
    const payload_t value{};
    const auto warm = codec.encode_parts (header, value, serializers);
    std::optional<msg::message_parts_t> parts;
    // Only the final two-part vector owns a C++ allocation on the typed path.
    EXPECT_EQ (
      count_allocations ([&] { parts.emplace (codec.encode_parts (header, value, serializers)); }),
      1u);
    auto decoded = codec.decode_header (*parts);
    ASSERT_TRUE (decoded);
    EXPECT_EQ (decoded.value ().metadata, header.metadata);
    EXPECT_EQ (decoded.value ().content_type, content_type);
    EXPECT_EQ ((*parts)[1].to_string (), "7");
    parts.reset ();
    // The erased registry additionally returns an owning content-type string.
    EXPECT_EQ (count_allocations ([&] {
                   parts.emplace (
                     codec.encode_parts (header, typeid (payload_t), &value, serializers));
               }),
               2u);
    EXPECT_EQ (header.content_type, "application/original");
    decoded = codec.decode_header (*parts);
    ASSERT_TRUE (decoded);
    EXPECT_EQ (decoded.value ().content_type, content_type);
    EXPECT_EQ (decoded.value ().correlation_id, header.correlation_id);
    const auto empty = codec.encode_parts (header, typeid (void), nullptr, serializers);
    decoded = codec.decode_header (empty);
    ASSERT_TRUE (decoded);
    EXPECT_EQ (decoded.value ().content_type, header.content_type);
    EXPECT_EQ (empty[1].size (), 0u);
}

TEST (EnvelopeAllocations, ReplyInitializesOnlyItsFinalFields)
{
    msg::envelope_header_t request;
    request.channel_name = "c";
    request.message_name = "m";
    request.content_type = "x/y";
    request.correlation_id = "id";
    request.metadata.emplace ("key", "value");
    fw::detail::channel_reply_writer_t writer;
    std::optional<msg::envelope_header_t> reply;
    EXPECT_EQ (count_allocations ([&] {
                   reply.emplace (
                     writer.create_reply_header (msg::message_kind_t::response, "c", request));
               }),
               0u);
    EXPECT_EQ (reply->kind, msg::message_kind_t::response);
    EXPECT_EQ (reply->channel_name, "c");
    EXPECT_EQ (reply->message_name, "m");
    EXPECT_EQ (reply->content_type, "x/y");
    EXPECT_EQ (reply->correlation_id, request.correlation_id);
    EXPECT_TRUE (reply->metadata.empty ());
}

TEST (EnvelopeAllocations, DecodeDoesNotAllocateADiscardedJsonDefault)
{
    msg::envelope_codec_t codec;
    const msg::envelope_header_t header{
      .channel_name = "c", .message_name = "m", .content_type = "x/y"};
    const auto wire = codec.encode_header (header);
    auto warm = codec.decode_header (wire, false);
    ASSERT_TRUE (warm);
    std::optional<decltype (warm)> decoded;
    // Parser diagnostics grow geometrically; the discarded JSON default alone
    // requests exactly strlen(application/json) + 1 bytes with libstdc++.
    const auto allocations = count_allocations (
      [&] { decoded.emplace (codec.decode_header (wire, false)); }, json_content_type.size () + 1);
    ASSERT_TRUE (*decoded);
    EXPECT_EQ (decoded->value ().content_type, "x/y");
    EXPECT_EQ (decoded->value ().channel_name, "c");
    EXPECT_EQ (decoded->value ().message_name, "m");
    EXPECT_EQ (allocations, 0u);
}

TEST (EnvelopeAllocations, ClientKeepsOneOwningJsonDefault)
{
    msg::client_call_codec_t codec;
    const std::string json (json_content_type);
    std::optional<std::string> copy;
    const auto final_string_allocations = count_allocations ([&] { copy.emplace (json); });
    std::optional<msg::envelope_header_t> header;
    EXPECT_EQ (count_allocations ([&] {
                   header.emplace (codec.create_envelope (msg::message_kind_t::command, "c", "m"));
               }),
               final_string_allocations);
    EXPECT_EQ (header->content_type, json);
    EXPECT_EQ (header->kind, msg::message_kind_t::command);
    EXPECT_EQ (header->channel_name, "c");
    EXPECT_EQ (header->message_name, "m");
    EXPECT_TRUE (header->correlation_id.empty ());
    EXPECT_FALSE (header->deadline);
}
} // namespace
