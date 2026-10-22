/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/channels/channel_packet_dispatcher.hpp"
#include <zlink/framework.hpp>
#include <cstdio>
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

struct allocation_handler_t
{
    unsigned calls = 0;
    void send (const payload_t &) { ++calls; }
    payload_t request (const payload_t &value)
    {
        ++calls;
        return value;
    }
};

TEST (EnvelopeAllocations, DisabledTypedChannelDispatchAllocationProbe)
{
    fw::zlink_builder_t builder;
    fw::dispatch_options_t options;
    options.message_flow (fw::message_flow_log_mode_t::off);
    fw::detail::apply_dispatch_options (builder, options);
    builder.channel ("c").enable_server ().bind ("inproc://channel-allocation-probe");
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    fw::detail::channel_packet_dispatcher_t dispatcher (runtime);
    fw::serializer_registry_t serializers;
    serializers.add<payload_t> (
      [] (const payload_t &) { return fw::encoded_payload_t::from_string ("7"); },
      [] (const fw::encoded_payload_t &) { return payload_t{}; }, "application/json");
    runtime.bind_serializers (serializers);
    fw::service_collection_t services;
    services.add_singleton<allocation_handler_t> ();
    auto provider = services.build_provider ();
    fw::handler_registry_t handlers;
    handlers.on_send<allocation_handler_t, payload_t> ("c", "", &allocation_handler_t::send,
                                                       {.packet_name = "m"});
    handlers.on_request<allocation_handler_t, payload_t, payload_t> (
      "c", "", &allocation_handler_t::request, {.packet_name = "r"});
    msg::envelope_header_t header;
    header.channel_name = "c";
    header.correlation_id = "corr";
    for (const bool request : {false, true}) {
        header.kind = request ? msg::message_kind_t::request : msg::message_kind_t::command;
        header.message_name = request ? "r" : "m";
        const auto parts = msg::envelope_codec_t{}.encode_parts (header, payload_t{}, serializers);
        bool succeeded = true;
        auto invoke = [&] {
            const auto result =
              dispatcher.dispatch_server_message ("c", parts, provider, serializers, handlers);
            succeeded = succeeded && static_cast<bool> (result);
            if (request && result) {
                const auto reply = msg::envelope_codec_t{}.decode_header (result.value ());
                succeeded =
                  succeeded && reply && reply.value ().kind == msg::message_kind_t::response;
            }
        };
        for (unsigned index = 0; index < 128; ++index)
            invoke ();
        const auto allocations = count_allocations ([&] {
            for (unsigned index = 0; index < 128; ++index)
                invoke ();
        });
        EXPECT_TRUE (succeeded);
        EXPECT_GT (allocations, 0u);
        std::printf ("Off typed Channel %s: %zu allocations / 128 calls.\n",
                     request ? "request" : "send", allocations);
    }
    EXPECT_EQ (512u, provider.get_required<allocation_handler_t> ().calls);
}

TEST (EnvelopeAllocations, DisabledChannelTraceFactoriesDoNotAllocate)
{
    fw::dispatch_options_t options;
    options.message_flow (fw::message_flow_log_mode_t::off);
    fw::detail::dispatch_options_access_t::set_observer_for_tests (options, [] (const auto &) {});
    fw::detail::message_flow_tracer_t flow (options);
    unsigned factories = 0;
    auto trace = [&] {
        for (const auto phase :
             {fw::message_flow_outcome_t::received, fw::message_flow_outcome_t::admitted,
              fw::message_flow_outcome_t::dispatched, fw::message_flow_outcome_t::completed,
              fw::message_flow_outcome_t::replied}) {
            flow.trace (phase, [&] {
                ++factories;
                return fw::message_flow_event_t{phase,
                                                fw::dispatch_error_surface_t::channel,
                                                fw::dispatch_message_kind_t::request,
                                                std::string (128, 'p'),
                                                std::nullopt,
                                                std::nullopt,
                                                std::nullopt,
                                                std::nullopt,
                                                std::nullopt,
                                                std::nullopt,
                                                std::nullopt};
            });
        }
    };
    EXPECT_EQ (0u, count_allocations (trace));
    EXPECT_EQ (0u, factories);
    options.message_flow (fw::message_flow_log_mode_t::normal);
    EXPECT_GT (count_allocations (trace), 0u);
    EXPECT_EQ (5u, factories);
}

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

// Container members (std::map metadata) may allocate even when empty, and how
// many allocations that costs differs by standard library (MSVC allocates a
// sentinel node). Measure a header whose strings fit inline as the baseline.
std::size_t header_container_baseline ()
{
    std::optional<msg::envelope_header_t> header;
    return count_allocations ([&] {
        header.emplace (
          msg::envelope_header_t{.channel_name = "c", .message_name = "m", .content_type = "x/y"});
    });
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
    const auto baseline = header_container_baseline ();
    EXPECT_EQ (count_allocations ([&] {
                   reply.emplace (
                     writer.create_reply_header (msg::message_kind_t::response, "c", request));
               }),
               baseline);
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
    const auto baseline = header_container_baseline ();
    std::optional<msg::envelope_header_t> header;
    EXPECT_EQ (count_allocations ([&] {
                   header.emplace (codec.create_envelope (msg::message_kind_t::command, "c", "m"));
               }),
               baseline + final_string_allocations);
    EXPECT_EQ (header->content_type, json);
    EXPECT_EQ (header->kind, msg::message_kind_t::command);
    EXPECT_EQ (header->channel_name, "c");
    EXPECT_EQ (header->message_name, "m");
    EXPECT_TRUE (header->correlation_id.empty ());
    EXPECT_FALSE (header->deadline);
}
} // namespace
