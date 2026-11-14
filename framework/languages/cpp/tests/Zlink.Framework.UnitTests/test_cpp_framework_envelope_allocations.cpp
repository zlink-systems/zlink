/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/json.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "allocation_counter.hpp"
#include "runtime_failure_fixture.hpp"
#include <cstring>
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/channels/channel_packet_dispatcher.hpp"
#include <zlink/framework.hpp>
#include <cstdio>
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/mesh/service_mailbox.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include <cstdlib>
#include <ctime>
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
}

template <> struct zlink::json::dto_traits<payload_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("value", &payload_t::value)};
};

namespace
{


struct allocation_handler_state_t
{
    unsigned calls = 0;
};

struct allocation_handler_t
{
    explicit allocation_handler_t (allocation_handler_state_t &state) : state (state) {}

    allocation_handler_state_t &state;
    void send (const payload_t &) { ++state.calls; }
    payload_t request (const payload_t &value)
    {
        ++state.calls;
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
    services.add_singleton<allocation_handler_state_t> ();
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
    EXPECT_EQ (512u, provider.get_required<allocation_handler_state_t> ().calls);
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
    header.correlation_id = std::string (32, 'b');
    EXPECT_EQ (
      count_allocations ([&] { parts.emplace (codec.encode_parts (header, value, serializers)); }),
      1u);
    decoded = codec.decode_header (*parts);
    ASSERT_TRUE (decoded);
    EXPECT_EQ (decoded.value ().correlation_id, header.correlation_id);
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

TEST (EnvelopeEncoding, RepeatedHeaderKeepsIndependentWritableBytes)
{
    msg::envelope_codec_t codec;
    msg::envelope_header_t header{.kind = msg::message_kind_t::command,
                                  .channel_name = "cached-channel",
                                  .message_name = "cached-message"};
    auto first = codec.encode_header (header);
    const auto expected = first.to_string ();
    auto second = codec.encode_header (header);
    ASSERT_EQ (second.to_string (), expected);
    first.bytes ()[0] = std::byte{};
    second.bytes ()[1] = std::byte{};
    EXPECT_EQ (codec.encode_header (header).to_string (), expected);
}

TEST (EnvelopeEncoding, ChangedValuesAndInvalidHeadersCannotReusePreviousBytes)
{
    msg::envelope_codec_t codec;
    const msg::envelope_header_t original{.kind = msg::message_kind_t::command,
                                          .channel_name = "cache-values",
                                          .message_name = "message"};
    using mutation_t = std::function<void (msg::envelope_header_t &)>;
    const mutation_t mutations[] = {[] (auto &h) { h.kind = msg::message_kind_t::publish; },
                                    [] (auto &h) { h.channel_name = "other-channel"; },
                                    [] (auto &h) { h.message_name = "other-message"; },
                                    [] (auto &h) { h.content_type = "other/type"; },
                                    [] (auto &h) { h.correlation_id = "correlation"; },
                                    [] (auto &h) { h.deadline = "2026-11-09T00:00:00Z"; },
                                    [] (auto &h) { h.topic = "topic"; },
                                    [] (auto &h) { h.error_code = "unavailable"; },
                                    [] (auto &h) { h.error_message = "error"; },
                                    [] (auto &h) { h.source = "source"; },
                                    [] (auto &h) { h.metadata = {{"k", "v"}}; },
                                    [] (auto &h) {
                                        h.flow_id = "019a55d5-0000-7000-8000-000000000001";
                                        h.flow_origin = fw::flow_origin_t::application;
                                    }};
    for (const auto &mutate : mutations) {
        const auto previous = codec.encode_header (original).to_string ();
        auto changed = original;
        mutate (changed);
        const auto bytes = codec.encode_header (changed).to_string ();
        EXPECT_NE (bytes, previous);
        EXPECT_EQ (codec.encode_header (changed).to_string (), bytes);
        EXPECT_EQ (codec.encode_header (original).to_string (), previous);
    }
    auto invalid = original;
    invalid.correlation_id = std::string (65, 'x');
    EXPECT_THROW (codec.encode_header (invalid), fw::framework_exception_t);
    invalid = original;
    invalid.metadata = {{"", "value"}};
    EXPECT_THROW (codec.encode_header (invalid), fw::framework_exception_t);
    invalid = original;
    invalid.flow_origin = fw::flow_origin_t::application;
    EXPECT_THROW (codec.encode_header (invalid), fw::framework_exception_t);
    const auto recovered = codec.encode_header (original).to_string ();
    EXPECT_EQ (nlohmann::json::parse (recovered).at ("channelName"), "cache-values");
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

void expect_same_header (const msg::envelope_header_t &actual,
                         const msg::envelope_header_t &expected)
{
    EXPECT_EQ (actual.kind, expected.kind);
    EXPECT_EQ (actual.channel_name, expected.channel_name);
    EXPECT_EQ (actual.message_name, expected.message_name);
    EXPECT_EQ (actual.content_type, expected.content_type);
    EXPECT_EQ (actual.correlation_id, expected.correlation_id);
    EXPECT_EQ (actual.deadline, expected.deadline);
    EXPECT_EQ (actual.topic, expected.topic);
    EXPECT_EQ (actual.error_code, expected.error_code);
    EXPECT_EQ (actual.error_message, expected.error_message);
    EXPECT_EQ (actual.source, expected.source);
    EXPECT_EQ (actual.metadata, expected.metadata);
    EXPECT_EQ (actual.flow_id, expected.flow_id);
    EXPECT_EQ (actual.flow_origin, expected.flow_origin);
}

std::string node_ordered_header (std::string_view wire)
{
    constexpr std::array<std::string_view, 14> node_field_order = {
      "formatMarker",  "kind",     "channelName", "messageName", "contentType",
      "correlationId", "deadline", "topic",       "errorCode",   "errorMessage",
      "source",        "metadata", "flowId",      "flowOrigin"};
    const auto parsed = nlohmann::json::parse (wire);
    auto ordered = nlohmann::ordered_json::object ();
    for (const auto field : node_field_order) {
        const std::string name (field);
        ordered.emplace (name, parsed.at (name));
    }
    return ordered.dump ();
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

TEST (EnvelopeAllocations, CanonicalHeaderDecodeAddsNoStorageBeyondHeaderOwnership)
{
    msg::envelope_codec_t codec;
    const msg::envelope_header_t header{
      .channel_name = "c", .message_name = "m", .content_type = "x/y"};
    const auto wire = codec.encode_header (header);
    auto warm = codec.decode_header (wire, false);
    ASSERT_TRUE (warm);
    std::optional<decltype (warm)> decoded;
    const auto allocations =
      count_allocations ([&] { decoded.emplace (codec.decode_header (wire, false)); });
    ASSERT_TRUE (*decoded);
    EXPECT_EQ (decoded->value ().channel_name, "c");
    EXPECT_EQ (decoded->value ().message_name, "m");
    EXPECT_EQ (decoded->value ().content_type, "x/y");
    // Only the final header and result moves own storage. MSVC allocates a
    // head node even for an empty map, so the reference must not construct
    // the SAX metadata owner on the canonical path.
    const auto reference = [] {
        msg::envelope_header_t value{
          .channel_name = "c", .message_name = "m", .content_type = "x/y"};
        return fw::result_t<msg::envelope_header_t>::success (std::move (value));
    };
    std::optional<decltype (warm)> reference_decoded;
    const auto reference_allocations =
      count_allocations ([&] { reference_decoded.emplace (reference ()); });
    ASSERT_TRUE (*reference_decoded);
    EXPECT_EQ (allocations, reference_allocations);
}

TEST (EnvelopeAllocations, CanonicalHeaderDecodeMatchesFallbackForEscapesAndUnicode)
{
    msg::envelope_codec_t codec;
    msg::envelope_header_t header{.kind = msg::message_kind_t::error,
                                  .channel_name = "orders\"\\\n",
                                  .message_name = "검색/😀",
                                  .content_type = "application/json",
                                  .correlation_id = "corr-17"};
    header.deadline = "2026-10-31T00:00:00Z\t";
    header.topic = "topic/한글";
    header.error_code = "internal_failure";
    header.error_message = std::string ("line\nzero\0end", 13);
    header.source = "node-é";
    header.metadata.emplace ("a\"key", std::string ("value\n\x01") + "한글");
    header.metadata.emplace ("z", "tail");
    header.flow_id = "01890a5d-ac96-774b-bcce-b302099a8057";
    header.flow_origin = zlink::framework::flow_origin_t::inbound;

    const auto wire = codec.encode_header (header).to_string ();
    const auto canonical_message = zlink::message_t::from (wire);
    const auto whitespace_message = zlink::message_t::from (" \n\t" + wire);
    auto canonical = codec.decode_header (canonical_message);
    auto whitespace_fallback = codec.decode_header (whitespace_message);
    auto reordered_fallback =
      codec.decode_header (zlink::message_t::from (node_ordered_header (wire)));
    auto canonical_reply = codec.decode_header (canonical_message, false);
    auto fallback_reply = codec.decode_header (whitespace_message, false);
    std::optional<decltype (canonical)> canonical_measurement;
    std::optional<decltype (canonical)> fallback_measurement;
    const auto canonical_allocations = count_allocations (
      [&] { canonical_measurement.emplace (codec.decode_header (canonical_message)); });
    const auto fallback_allocations = count_allocations (
      [&] { fallback_measurement.emplace (codec.decode_header (whitespace_message)); });
    ASSERT_TRUE (canonical);
    ASSERT_TRUE (whitespace_fallback);
    ASSERT_TRUE (reordered_fallback);
    ASSERT_TRUE (canonical_reply);
    ASSERT_TRUE (fallback_reply);
    ASSERT_TRUE (*canonical_measurement);
    ASSERT_TRUE (*fallback_measurement);
    EXPECT_LT (canonical_allocations, fallback_allocations);
    std::printf ("Escaped canonical header decode: %zu allocations; SAX fallback: %zu.\n",
                 canonical_allocations, fallback_allocations);
    expect_same_header (canonical.value (), whitespace_fallback.value ());
    expect_same_header (canonical.value (), reordered_fallback.value ());
    expect_same_header (canonical_reply.value (), fallback_reply.value ());
    EXPECT_FALSE (canonical_reply.value ().flow_id);
    EXPECT_FALSE (canonical_reply.value ().flow_origin);
}

TEST (EnvelopeAllocations, MissingOptionalFieldsMatchCanonicalDefaults)
{
    msg::envelope_codec_t codec;
    const msg::envelope_header_t expected{
      .kind = msg::message_kind_t::command, .channel_name = "c", .message_name = "m"};
    const auto canonical = codec.decode_header (codec.encode_header (expected));
    const auto missing_optional = codec.decode_header (zlink::message_t::from (
      R"({"formatMarker":242,"kind":3,"channelName":"c","messageName":"m"})"));
    ASSERT_TRUE (canonical);
    ASSERT_TRUE (missing_optional);
    expect_same_header (canonical.value (), missing_optional.value ());
}

TEST (EnvelopeAllocations, MalformedCanonicalPrefixKeepsJsonParserError)
{
    msg::envelope_codec_t codec;
    const msg::envelope_header_t header{
      .channel_name = "c", .message_name = "m", .content_type = "x/y"};
    auto malformed = codec.encode_header (header).to_string ();
    malformed.pop_back ();
    std::string json_error;
    try {
        json_error = nlohmann::json::parse (malformed).dump ();
    }
    catch (const nlohmann::json::parse_error &error) {
        json_error = error.what ();
    }

    const auto decoded = codec.decode_header (zlink::message_t::from (malformed));
    ASSERT_FALSE (decoded);
    ASSERT_TRUE (decoded.error ());
    EXPECT_EQ (decoded.error_kind (), zlink::framework::framework_error_kind_t::protocol_error);
    EXPECT_EQ (std::string (decoded.error ()->what ()),
               std::string ("invalid ZLink envelope header: ") + json_error);
}

TEST (EnvelopeAllocations, MailboxRecordDoesNotConstructAnUnusedContentType)
{
    using record_t = fw::runtime::mesh::service_mailbox_record_t;
    using header_t = fw::runtime::messaging::envelope_header_t;
    alignas (header_t) std::byte header_storage[sizeof (header_t)];
    alignas (record_t) std::byte actual_storage[sizeof (record_t)];
    header_t *header = nullptr;
    record_t *actual = nullptr;
    // The baseline is the record's header member constructed the same way, alone. An empty
    // std::map allocates its head node on MSVC and not on libstdc++, so the count is the
    // platform's container cost; a default content type string would add to the record only.
    const auto container_allocations = count_allocations (
      [&] { header = ::new (static_cast<void *> (header_storage)) header_t{.content_type = {}}; });
    const auto allocations =
      count_allocations ([&] { actual = ::new (static_cast<void *> (actual_storage)) record_t{}; });
    EXPECT_EQ (allocations, container_allocations);
    EXPECT_TRUE (actual->envelope_header.content_type.empty ());
    EXPECT_TRUE (actual->parts ().empty ());
    std::destroy_at (header);
    std::destroy_at (actual);
}

TEST (EnvelopeAllocations, NodeSendOwnsOnlyTheSerializedContentType)
{
    fw::serializer_registry_t serializers;
    const std::string content_type = "application/x-protobuf";
    serializers.add<payload_t> (
      [] (const payload_t &) {
          return fw::encoded_payload_t::from_string (std::string (1027, 'p'));
      },
      [] (const fw::encoded_payload_t &) { return payload_t{}; }, content_type);
    const auto serializer = serializers.get<payload_t> ();
    const auto target = zlink::routing_id_t::from ("bench-server");
    fw::dispatch_options_t dispatch;
    dispatch.message_flow (fw::message_flow_log_mode_t::off);
    const std::map<std::string, std::string> metadata;
    msg::client_call_codec_t codec;
    msg::envelope_codec_t envelope;
    const auto encode = [&] { return serializer.serialize_with_content_type (payload_t{}); };
    const auto encode_baseline = [&] {
        auto serialized = encode ();
        msg::envelope_header_t header{.kind = msg::message_kind_t::command,
                                      .channel_name = "bench",
                                      .message_name = "BenchPayload",
                                      .content_type = std::move (serialized.content_type)};
        return envelope.encode_raw_body_parts (
          header, fw::detail::encoded_payload_to_raw (serialized.payload));
    };
    const auto encode_node = [&] {
        return codec.encode_node_send_parts ("bench", "BenchPayload", target, metadata, dispatch,
                                             serializers, [&] (auto &) { return encode (); });
    };
    (void) encode_baseline ();
    (void) encode_node ();
    std::optional<msg::message_parts_t> expected;
    const auto baseline = count_allocations ([&] { expected.emplace (encode_baseline ()); });
    std::optional<msg::message_parts_t> actual;
    const auto allocations = count_allocations ([&] { actual.emplace (encode_node ()); });
    EXPECT_EQ (allocations, baseline);
    ASSERT_TRUE (expected);
    ASSERT_TRUE (actual);
    EXPECT_EQ ((*actual)[0].to_string (), (*expected)[0].to_string ());
    EXPECT_EQ ((*actual)[1].to_string (), (*expected)[1].to_string ());
    auto decoded = envelope.decode_header (*actual, false);
    ASSERT_TRUE (decoded);
    EXPECT_EQ (decoded.value ().content_type, content_type);
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

TEST (EnvelopeDeadline, IntegerUtcMatchesCalendarBoundaries)
{
    using namespace std::chrono;
    // Reference the original formatter over the full system_clock date range,
    // including leap centuries and negative fractional epochs.
#if defined(_WIN32)
    constexpr int first_year = 1970; // gmtime_s rejects negative time_t values.
#else
    constexpr int first_year = 1680;
#endif
    for (int year = first_year; year <= 2260; ++year) {
        for (unsigned month = 1; month <= 12; ++month) {
            for (int day : {1, 28}) {
                for (int offset : {-1, 0, 1, 86399, 86400}) {
                    const auto value = sys_days (std::chrono::year (year) / month / day)
                                       + seconds (offset) + milliseconds (123);
                    const auto truncated = time_point_cast<seconds> (value);
                    const auto time = system_clock::to_time_t (truncated);
                    std::tm utc{};
#if defined(_WIN32)
                    gmtime_s (&utc, &time);
#else
                    gmtime_r (&time, &utc);
#endif
                    char expected[64];
                    const auto size =
                      std::strftime (expected, sizeof (expected), "%Y-%m-%dT%H:%M:%SZ", &utc);
                    ASSERT_EQ (msg::format_utc_deadline (value), std::string (expected, size));
                }
            }
        }
    }
}

TEST (EnvelopeAllocations, CsitesHeaderViewsRetainWireAcrossCopiesAndSuspension)
{
    namespace msg = zlink::framework::runtime::messaging;
    msg::envelope_codec_t codec;
    const std::string packet (128, 'p');
    std::optional<msg::envelope_header_t> short_retained;
    auto short_wire = codec.encode_header (
      msg::envelope_header_t{.channel_name = "c", .message_name = "m", .content_type = "x/y"});
    const auto container_allocations = zlink::framework::tests::count_allocations ([&] {
        auto decoded = codec.decode_header_view (short_wire, false);
        ASSERT_TRUE (decoded);
        decoded.value ().retained_header.message = std::move (short_wire);
        short_retained.emplace (std::move (decoded.value ()));
    });
    std::optional<msg::envelope_header_t> retained;
    {
        msg::envelope_header_t header{.channel_name = "mesh",
                                      .message_name = packet,
                                      .content_type = "application/json",
                                      .correlation_id = std::string (48, 'c')};
        auto wire = codec.encode_header (header);
        auto warm = codec.decode_header (wire, false);
        ASSERT_TRUE (warm);
        const auto start = reinterpret_cast<std::uintptr_t> (wire.data ());
        const auto allocations = zlink::framework::tests::count_allocations ([&] {
            auto decoded = codec.decode_header_view (wire, false);
            ASSERT_TRUE (decoded);
            decoded.value ().retained_header.message = std::move (wire);
            retained.emplace (std::move (decoded.value ()));
        });
        EXPECT_EQ (allocations, container_allocations);
        std::printf ("csites header views=%zu container=%zu\n", allocations, container_allocations);
        const auto text = reinterpret_cast<std::uintptr_t> (retained->message_name.data ());
        EXPECT_GE (text, start);
        EXPECT_LT (text, start + retained->retained_header.message->size ());
    }
    *retained = *retained;
    EXPECT_TRUE (retained->retained_header.message.has_value ());
    EXPECT_EQ (retained->message_name, packet);
    auto moved = std::move (*retained);
    retained.reset ();
    EXPECT_EQ (moved.message_name, packet);
    auto copied = moved;
    moved = {};
    EXPECT_EQ (copied.message_name, packet);
    auto reply = zlink::framework::detail::channel_reply_writer_t{}.create_reply_header (
      msg::message_kind_t::response, "mesh", copied);
    EXPECT_EQ (reply.message_name.data (), copied.message_name.data ());
    const auto wire = codec.encode_header (reply);
    EXPECT_EQ (codec.decode_header (wire).value ().message_name, packet);
}

TEST (EnvelopeDeadline, CsitesInlineDeadlineSurvivesHeaderMoveAndCopy)
{
    namespace msg = zlink::framework::runtime::messaging;
    msg::client_call_codec_t calls;
    auto first = calls.create_envelope (msg::message_kind_t::request, "mesh", "packet",
                                        std::chrono::seconds (30));
    ASSERT_TRUE (first.deadline);
    const std::string expected (*first.deadline);
    auto moved = std::move (first);
    first = {};
    auto copied = moved;
    moved = {};
    EXPECT_EQ (std::string (*copied.deadline), expected);
    const auto wire = msg::envelope_codec_t{}.encode_header (copied);
    EXPECT_EQ (std::string (*msg::envelope_codec_t{}.decode_header (wire).value ().deadline),
               expected);
}


// Enable only in the separately linked copy probe; ordinary CTest uses the same
// allocation assertions without interposing process-wide library calls.
#ifdef ZLINK_TEST_L2_COPY_PROBE
namespace
{
thread_local bool l2_count_copies = false;
thread_local std::size_t l2_copy_calls = 0;
thread_local std::size_t l2_copy_bytes = 0;
thread_local std::size_t l2_native_buffers = 0;
}
extern "C" void *__real_memcpy (void *, const void *, std::size_t);
extern "C" void *__wrap_memcpy (void *destination, const void *source, std::size_t size)
{
    if (l2_count_copies && size != 0) {
        ++l2_copy_calls;
        l2_copy_bytes += size;
    }
    return __real_memcpy (destination, source, size);
}
extern "C" zlink_config_result_t __real_zlink_msg_init_size (zlink_msg_t *, std::size_t);
extern "C" zlink_config_result_t __wrap_zlink_msg_init_size (zlink_msg_t *message, std::size_t size)
{
    if (l2_count_copies)
        ++l2_native_buffers;
    return __real_zlink_msg_init_size (message, size);
}
#endif

#ifdef ZLINK_TEST_L2_PASS_PROBE
extern "C" void zlink_test_l2_reset_passes ();
extern "C" std::size_t zlink_test_l2_scan_bytes ();
extern "C" std::size_t zlink_test_l2_metadata_checks ();

namespace
{
TEST (EnvelopeL2Pass, CanonicalStringsShareUtf8AndSyntaxTraversal)
{
    msg::envelope_codec_t codec;
    for (const std::string suffix : {std::string ("ascii"), std::string ("é한😀")}) {
        msg::envelope_header_t header{.channel_name = std::string (48, 'c') + suffix,
                                      .message_name = std::string (48, 'm') + suffix,
                                      .correlation_id = std::string (64, 'i'),
                                      .deadline = "2026-11-14T00:00:00Z"};
        const auto wire = codec.encode_header (header);
        const auto string_bytes = header.channel_name.size () + header.message_name.size ()
                                  + header.content_type.size () + header.correlation_id.size ()
                                  + header.deadline->size ();
        zlink_test_l2_reset_passes ();
        std::optional<fw::result_t<msg::envelope_header_t>> decoded;
        const auto allocations =
          count_allocations ([&] { decoded.emplace (codec.decode_header_view (wire, false)); });
        ASSERT_TRUE (*decoded);
        EXPECT_EQ (zlink_test_l2_scan_bytes (), string_bytes);
        EXPECT_EQ (allocations, 0u);
        std::printf ("L2_PASS canonical-%s scan_bytes=%zu string_bytes=%zu new=%zu\n",
                     suffix == "ascii" ? "ascii" : "unicode", zlink_test_l2_scan_bytes (),
                     string_bytes, allocations);
        expect_same_header (decoded->value (), header);
    }
}

TEST (EnvelopeL2Pass, EncodeValidatesDuringSizingAndOnlyEmitsDuringWriting)
{
    msg::envelope_codec_t codec;
    msg::client_call_codec_t calls;
    const auto channel = std::make_shared<const fw::detail::prepared_header_fragment_t> (
      fw::detail::prepare_request_channel ("bench"));
    const auto type = std::make_shared<const fw::detail::request_type_plan_t> (
      fw::detail::prepare_request_type ("BenchPayload", "application/json"));
    for (const bool prepared : {false, true}) {
        msg::envelope_header_t header{.kind = prepared ? msg::message_kind_t::request
                                                       : msg::message_kind_t::response,
                                      .channel_name = "bench",
                                      .message_name = "BenchPayload",
                                      .correlation_id = std::string (64, 'i'),
                                      .deadline = "2026-11-14T00:00:00Z",
                                      .topic = std::string (48, 't') + "é한😀"};
        header.metadata.emplace ("key", std::string (48, 'v'));
        (void) codec.encode_header (header);
        header.correlation_id = std::string (64, 'j');
        const auto string_bytes =
          header.correlation_id.size () + header.deadline->size () + header.topic->size () + 3 + 48;
        zlink_test_l2_reset_passes ();
        const auto expected = header;
        std::optional<msg::message_parts_t> wire;
        const auto allocations = count_allocations ([&] {
            wire.emplace (prepared
                            ? calls.encode_payload_parts (std::move (header),
                                                          fw::encoded_payload_t::from_string ("7"),
                                                          type, channel)
                            : codec.encode_raw_body_parts (header, zlink::message_t::from ("7")));
        });
        const auto scanned = zlink_test_l2_scan_bytes ();
        const auto checks = zlink_test_l2_metadata_checks ();
        EXPECT_EQ (scanned, 2 * string_bytes);
        EXPECT_EQ (checks, 1u);
        EXPECT_EQ (allocations, 1u);
        std::printf (
          "L2_PASS encode-%s scan_bytes=%zu string_bytes=%zu metadata_checks=%zu new=%zu\n",
          prepared ? "prepared" : "warm-response-path", scanned, string_bytes, checks, allocations);
        ASSERT_TRUE (codec.decode_header (*wire, false));
        expect_same_header (codec.decode_header (*wire, false).value (), expected);
    }
}
} // namespace
#endif

namespace
{
template <typename Action> std::size_t measure_l2 (const char *path, Action &&action)
{
#ifdef ZLINK_TEST_L2_COPY_PROBE
    l2_copy_calls = 0;
    l2_copy_bytes = 0;
    l2_native_buffers = 0;
    l2_count_copies = true;
#endif
    const auto allocations = count_allocations (std::forward<Action> (action));
#ifdef ZLINK_TEST_L2_COPY_PROBE
    l2_count_copies = false;
    std::printf ("L2_COST %s new=%zu memcpy=%zu bytes=%zu native_buffers=%zu\n", path, allocations,
                 l2_copy_calls, l2_copy_bytes, l2_native_buffers);
#else
    std::printf ("L2_COST %s new=%zu\n", path, allocations);
#endif
    return allocations;
}

TEST (EnvelopeL2Cost, SynchronousSendBorrowsNamesUntilEncodingFinishes)
{
    fw::serializer_registry_t serializers;
    serializers.add<payload_t> (
      [] (const payload_t &) { return fw::encoded_payload_t::from_string ("7"); },
      [] (const fw::encoded_payload_t &) { return payload_t{}; }, "application/json");
    for (const bool long_names : {false, true}) {
        const std::string channel = long_names ? std::string (48, 'c') : "bench";
        const std::string packet = long_names ? std::string (48, 'm') : "BenchPayload";
        const auto target = zlink::routing_id_t::from ("n");
        const std::map<std::string, std::string> metadata;
        fw::dispatch_options_t options;
        options.message_flow (fw::message_flow_log_mode_t::off);
        msg::client_call_codec_t calls;
        msg::envelope_codec_t codec;
        auto serialize = [&] {
            return serializers.get<payload_t> ().serialize_with_content_type (payload_t{});
        };
        auto reference = [&] {
            auto serialized = serialize ();
            const msg::envelope_header_t header{.kind = msg::message_kind_t::command,
                                                .channel_name = msg::header_text_t::view (channel),
                                                .message_name = msg::header_text_t::view (packet),
                                                .content_type =
                                                  std::move (serialized.content_type)};
            return codec.encode_raw_body_parts (
              header, fw::detail::encoded_payload_to_raw (std::move (serialized.payload)));
        };
        auto send = [&] {
            return calls.encode_node_send_parts (channel, packet, target, metadata, options,
                                                 serializers,
                                                 [&] (auto &) { return serialize (); });
        };
        (void) reference ();
        (void) send ();
        std::optional<msg::message_parts_t> expected;
        const auto baseline = count_allocations ([&] { expected.emplace (reference ()); });
        std::optional<msg::message_parts_t> actual;
        const auto allocations =
          measure_l2 (long_names ? "command-send-long" : "command-send-short",
                      [&] { actual.emplace (send ()); });
        EXPECT_EQ (allocations, baseline);
        ASSERT_TRUE (actual);
        EXPECT_EQ ((*actual)[0].to_string (), (*expected)[0].to_string ());
        EXPECT_EQ ((*actual)[1].to_string (), "7");
    }
}

TEST (EnvelopeL2Cost, CompletionBorrowsNativeAndSpanBackedHeaderText)
{
    fw::serializer_registry_t serializers;
    serializers.add<payload_t> (
      [] (const payload_t &) { return fw::encoded_payload_t::from_string ("7"); },
      [] (const fw::encoded_payload_t &body) {
          if (std::string_view (reinterpret_cast<const char *> (body.bytes ().data ()),
                                body.bytes ().size ())
              != "7")
              throw fw::framework_exception_t (fw::framework_error_kind_t::protocol_error,
                                               "bad body");
          return payload_t{};
      },
      "application/json");
    const msg::envelope_header_t header{.kind = msg::message_kind_t::response,
                                        .channel_name = std::string (48, 'c'),
                                        .message_name = std::string (48, 'm'),
                                        .correlation_id = std::string (64, 'i')};
    const auto parts =
      msg::envelope_codec_t{}.encode_raw_body_parts (header, zlink::message_t::from ("7"));
    const std::array<std::span<const std::byte>, 2> spans{parts.part_bytes (0),
                                                          parts.part_bytes (1)};
    const auto borrowed = msg::message_parts_t::borrow (spans);
    msg::client_call_codec_t calls;
    const std::string empty = "empty";
    const std::string error = "error";
    const std::string operation = "request";
    const auto baseline = count_allocations ([&] {
        const auto view = msg::envelope_codec_t{}.decode_header_view (parts, false);
        EXPECT_TRUE (view);
    });
    for (const auto *reply : {&parts, &borrowed}) {
        ASSERT_TRUE (
          calls.decode_envelope_reply<payload_t> (*reply, serializers, empty, error, operation));
        std::optional<fw::result_t<payload_t>> decoded;
        const auto allocations =
          measure_l2 (reply == &parts ? "completion-native" : "completion-span", [&] {
              decoded.emplace (calls.decode_envelope_reply<payload_t> (*reply, serializers, empty,
                                                                       error, operation));
          });
        ASSERT_TRUE (*decoded);
        EXPECT_EQ (decoded->value ().value, 7);
        EXPECT_EQ (allocations, baseline);
    }
}

TEST (EnvelopeL2Cost, RequestTargetAndResponseCosts)
{
    msg::client_call_codec_t calls;
    msg::envelope_codec_t codec;
    msg::envelope_header_t header{.channel_name = "bench",
                                  .message_name = "BenchPayload",
                                  .correlation_id = "42",
                                  .deadline = "2026-11-14T00:00:00Z"};
    auto channel = std::make_shared<const fw::detail::prepared_header_fragment_t> (
      fw::detail::prepare_request_channel ("bench"));
    auto type = std::make_shared<const fw::detail::request_type_plan_t> (
      fw::detail::prepare_request_type ("BenchPayload", "application/json"));
    auto outbound = header;
    std::optional<msg::message_parts_t> request;
    measure_l2 ("request-prepared", [&] {
        request.emplace (calls.encode_payload_parts (
          std::move (outbound), fw::encoded_payload_t::from_string ("7"), type, channel));
    });
    ASSERT_TRUE (request);
    std::optional<fw::result_t<msg::envelope_header_t>> received;
    measure_l2 ("target-view",
                [&] { received.emplace (codec.decode_header_view (*request, false)); });
    ASSERT_TRUE (*received);
    fw::detail::channel_reply_writer_t writer;
    (void) writer.reply_raw_envelope (
      writer.create_reply_header (msg::message_kind_t::response, "bench", received->value ()),
      zlink::message_t::from ("7"));
    std::optional<msg::message_parts_t> response;
    measure_l2 ("response-warm", [&] {
        response.emplace (writer.reply_raw_envelope (
          writer.create_reply_header (msg::message_kind_t::response, "bench", received->value ()),
          zlink::message_t::from ("7")));
    });
    ASSERT_TRUE (response);
    EXPECT_EQ (codec.decode_header (*response, false).value ().correlation_id, "42");
}

struct l2_runtime_payload_t
{
    static constexpr const char *packet_name =
      "envelope-cost-payload-with-a-long-production-packet-name";
    int value = 7;
};

TEST (EnvelopeL2Cost, ProductionNodeRequestKeepsPreparedNamesAndCompletes)
{
    std::size_t allocations_at_serializer = 0;
    fw::serializer_registry_t serializers;
    serializers.add<l2_runtime_payload_t> (
      [&] (const l2_runtime_payload_t &) {
          allocations_at_serializer = fw::tests::tracked_allocation_count;
          return fw::encoded_payload_t::from_string ("7");
      },
      [] (const fw::encoded_payload_t &) { return l2_runtime_payload_t{}; }, "application/json");
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    const std::string channel = "envelope-cost-channel-with-a-long-production-channel-name";
    const auto target = zlink::routing_id_t::from ("n");
    unsigned received = 0;
    auto completed =
      [] (msg::message_parts_t parts) -> fw::request_task_t<fw::result_t<msg::message_parts_t>> {
        co_return fw::result_t<msg::message_parts_t>::success (std::move (parts));
    };
    runtime.bind_mesh_node_transport (
      channel, {}, [&] (const auto &, auto parts, auto, auto receiver) {
          const auto decoded = msg::envelope_codec_t{}.decode_header_view (parts, false);
          ASSERT_TRUE (decoded);
          EXPECT_EQ (decoded.value ().channel_name, channel);
          EXPECT_EQ (decoded.value ().message_name, l2_runtime_payload_t::packet_name);
          EXPECT_EQ (decoded.value ().content_type, "application/json");
          EXPECT_EQ (decoded.value ().kind, msg::message_kind_t::request);
          EXPECT_TRUE (decoded.value ().deadline);
          EXPECT_FALSE (decoded.value ().correlation_id.empty ());
          ++received;
          fw::detail::channel_reply_writer_t writer;
          auto reply = writer.reply_raw_envelope (
            writer.create_reply_header (msg::message_kind_t::response, channel, decoded.value ()),
            zlink::message_t::from ("7"));
          receiver.observe (completed (std::move (reply)));
      });
    auto client = builder.route_client (serializers);
    EXPECT_EQ (client.request_to_node (channel, target, l2_runtime_payload_t{})
                 .async<l2_runtime_payload_t> ()
                 .result ()
                 .value ()
                 .value,
               7);
    std::optional<fw::result_t<l2_runtime_payload_t>> result;
    measure_l2 ("production-node-roundtrip", [&] {
        result.emplace (client.request_to_node (channel, target, l2_runtime_payload_t{})
                          .async<l2_runtime_payload_t> ()
                          .result ());
    });
    std::printf ("L2_STAGE request-before-serializer new=%zu\n", allocations_at_serializer);
    ASSERT_TRUE (*result);
    EXPECT_EQ (result->value ().value, 7);
    EXPECT_EQ (received, 2u);
}

#ifdef ZLINK_TEST_L2_COPY_PROBE
TEST (EnvelopeL2Cost, CopyCounterObservesEmittedCopies)
{
    std::array<char, 48> source{};
    std::array<char, 48> destination{};
    auto *volatile copy = &std::memcpy;
    const auto allocations = measure_l2 (
      "counter-control", [&] { copy (destination.data (), source.data (), source.size ()); });
    EXPECT_EQ (allocations, 0u);
    EXPECT_EQ (l2_copy_calls, 1u);
    EXPECT_EQ (l2_copy_bytes, source.size ());
    EXPECT_EQ (source, destination);
}
#endif

nlohmann::json l2_wire_fixture ()
{
    // Frozen encoder output from the unmodified #4352 base, including field order.
    return nlohmann::json::parse (
      R"({"channelName":"orders","contentType":"application/json","correlationId":null,"deadline":null,"errorCode":null,"errorMessage":null,"flowId":null,"flowOrigin":null,"formatMarker":242,"kind":1,"messageName":"payload","metadata":{},"source":null,"topic":null})");
}

TEST (EnvelopeL2Wire, EncoderMatchesFrozenBytesAcrossKindsFlowsAndCorrelationBoundaries)
{
    msg::envelope_codec_t codec;
    for (const auto kind : {msg::message_kind_t::request, msg::message_kind_t::response,
                            msg::message_kind_t::command}) {
        for (const bool flow : {false, true}) {
            for (const auto length : {0u, 1u, 15u, 16u, 64u}) {
                auto expected = l2_wire_fixture ();
                expected["kind"] = static_cast<int> (kind);
                msg::envelope_header_t header{
                  .kind = kind, .channel_name = "orders", .message_name = "payload"};
                if (length != 0) {
                    header.correlation_id = std::string (length, 'i');
                    expected["correlationId"] = std::string (length, 'i');
                }
                if (flow) {
                    header.flow_id = "019a55d5-0000-7000-8000-000000000001";
                    header.flow_origin = fw::flow_origin_t::application;
                    expected["flowId"] = *header.flow_id;
                    expected["flowOrigin"] = 3;
                }
                const auto actual = codec.encode_header (header).to_string ();
                EXPECT_EQ (actual, expected.dump ());
                std::printf ("L2_WIRE %s\n", actual.c_str ());
            }
        }
    }
}

TEST (EnvelopeL2Wire, BorrowedDecodePreservesRejectionsAndErrorText)
{
    msg::envelope_codec_t codec;
    auto fixture = l2_wire_fixture ();
    fixture["correlationId"] = "id";
    const auto valid = fixture.dump ();
    std::vector<std::string> inputs;
    for (std::size_t length = 0; length < valid.size (); ++length)
        inputs.push_back (valid.substr (0, length));
    for (const auto &correlation :
         {std::string (65, 'x'), std::string ("é"), std::string ("\\q"), std::string ("\\uD800")}) {
        const auto begin = valid.find ("\"id\"");
        auto invalid = valid;
        invalid.replace (begin, 4, "\"" + correlation + "\"");
        inputs.push_back (std::move (invalid));
    }
    inputs.push_back (valid + "x");
    for (const bool capture_flow : {false, true}) {
        for (const auto &input : inputs) {
            const auto wire = zlink::message_t::from (input);
            const auto owned = codec.decode_header (wire, capture_flow);
            const auto view = codec.decode_header_view (wire, capture_flow);
            const std::array<std::span<const std::byte>, 1> spans{wire.bytes ()};
            const auto span =
              codec.decode_header_view (msg::message_parts_t::borrow (spans), capture_flow);
            ASSERT_FALSE (owned);
            ASSERT_FALSE (view);
            ASSERT_FALSE (span);
            EXPECT_EQ (owned.error_kind (), view.error_kind ());
            EXPECT_EQ (owned.error_kind (), span.error_kind ());
            EXPECT_STREQ (owned.error ()->what (), view.error ()->what ());
            EXPECT_STREQ (owned.error ()->what (), span.error ()->what ());
            std::printf ("L2_REJECT %s\n", owned.error ()->what ());
        }
    }
}

TEST (EnvelopeL2Wire, UnicodeAndMetadataKeepFrozenDiagnostics)
{
    msg::envelope_codec_t codec;
    // Parser diagnostics may contain the malformed input bytes themselves.
    // Hex preserves every byte without asking a JSON serializer to validate it.
    const auto diagnostic_bytes = [] (std::string_view value) {
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve (2 * value.size ());
        for (const unsigned char byte : value) {
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
        return result;
    };
    const std::vector<std::string> values{"plain",
                                          "é한😀",
                                          "\\u0000",
                                          "\\u001f",
                                          "\\n",
                                          "\\/",
                                          "\\uD83D\\uDE00",
                                          "\\uD800",
                                          "\\uDC00",
                                          "\\q",
                                          std::string (1, '\0'),
                                          std::string ("\xc2", 1),
                                          std::string ("\x80", 1),
                                          std::string ("\xc0\xaf", 2),
                                          std::string ("\xe0\x9f\x80", 3),
                                          std::string ("\xed\xa0\x80", 3),
                                          std::string ("\xf0\x8f\x80\x80", 4),
                                          std::string ("\xf4\x90\x80\x80", 4),
                                          std::string ("\xf5\x80\x80\x80", 4),
                                          std::string ("\xe2\x82", 2),
                                          std::string ("\xe0\xa0\x80", 3),
                                          std::string ("\xed\x9f\xbf", 3),
                                          std::string ("\xf0\x90\x80\x80", 4),
                                          std::string ("\xf4\x8f\xbf\xbf", 4)};
    std::size_t index = 0;
    for (const auto *field :
         {"channelName", "messageName", "contentType", "correlationId", "deadline", "topic",
          "errorCode", "errorMessage", "source", "flowId", "metadata-key", "metadata-value"}) {
        for (const auto &value : values) {
            auto fixture = l2_wire_fixture ();
            fixture["correlationId"] = "id";
            if (std::string_view (field) == "metadata-key")
                fixture["metadata"] = {{"SENTINEL", "value"}};
            else if (std::string_view (field) == "metadata-value")
                fixture["metadata"] = {{"key", "SENTINEL"}};
            else
                fixture[field] = "SENTINEL";
            auto input = fixture.dump ();
            input.replace (input.find ("SENTINEL"), 8, value);
            for (const bool capture_flow : {false, true}) {
                const auto wire = zlink::message_t::from (input);
                const auto owned = codec.decode_header (wire, capture_flow);
                const auto view = codec.decode_header_view (wire, capture_flow);
                ASSERT_EQ (static_cast<bool> (owned), static_cast<bool> (view));
                if (owned)
                    expect_same_header (owned.value (), view.value ());
                else
                    EXPECT_STREQ (owned.error ()->what (), view.error ()->what ());
                const auto outcome =
                  owned ? std::string ("accepted") : std::string (owned.error ()->what ());
                std::printf ("L2_DIAG decode-%zu-%d %s\n", index, capture_flow,
                             diagnostic_bytes (outcome).c_str ());
            }
            ++index;
        }
    }
    for (const auto &value : values) {
        for (const auto field : {0, 1, 2, 3, 4}) {
            msg::envelope_header_t header{.channel_name = "bench", .message_name = "Payload"};
            switch (field) {
                case 0:
                    header.channel_name = value;
                    break;
                case 1:
                    header.topic = value;
                    break;
                case 2:
                    header.metadata.emplace (value, "value");
                    break;
                case 3:
                    header.metadata.emplace ("key", value);
                    break;
                case 4:
                    header.error_message = "before\n" + value;
                    break;
            }
            std::string outcome;
            try {
                outcome = codec.encode_header (header).to_string ();
                // Successful encoding must retain the JSON library's bytes.
                auto expected = l2_wire_fixture ();
                expected["channelName"] = std::string (header.channel_name);
                expected["messageName"] = "Payload";
                if (header.topic)
                    expected["topic"] = *header.topic;
                if (header.error_message)
                    expected["errorMessage"] = *header.error_message;
                expected["metadata"] = header.metadata;
                EXPECT_EQ (outcome, expected.dump ());
            }
            catch (const std::exception &error) {
                outcome = error.what ();
            }
            std::printf ("L2_DIAG encode-%zu-%d %s\n", index, field,
                         diagnostic_bytes (outcome).c_str ());
        }
        ++index;
    }
    for (const auto &metadata :
         {std::map<std::string, std::string>{{"", "value"}},
          std::map<std::string, std::string>{{std::string ("k\0", 2), "value"}},
          std::map<std::string, std::string>{{"key", std::string ("v\0", 2)}},
          std::map<std::string, std::string>{{"key", std::string (4096, 'v')}}}) {
        auto fixture = l2_wire_fixture ();
        fixture["metadata"] = metadata;
        const auto decoded = codec.decode_header_view (zlink::message_t::from (fixture.dump ()));
        ASSERT_FALSE (decoded);
        std::printf ("L2_DIAG metadata-%zu %s\n", index++,
                     diagnostic_bytes (decoded.error ()->what ()).c_str ());
    }
}

TEST (EnvelopeL2Wire, TrailingNulKeepsFrozenBaselineOutcome)
{
    // The unmodified decoder accepts this input through its existing SAX path.
    // This fixture preserves the requested rejection set; the grammar mismatch
    // is recorded separately because this change must not alter error policy.
    const auto input = l2_wire_fixture ().dump () + std::string (1, char{});
    const auto wire = zlink::message_t::from (input);
    const std::array<std::span<const std::byte>, 1> spans{wire.bytes ()};
    msg::envelope_codec_t codec;
    for (const bool capture_flow : {false, true}) {
        const auto owned = codec.decode_header (wire, capture_flow);
        const auto native = codec.decode_header_view (wire, capture_flow);
        const auto borrowed =
          codec.decode_header_view (msg::message_parts_t::borrow (spans), capture_flow);
        ASSERT_TRUE (owned);
        ASSERT_TRUE (native);
        ASSERT_TRUE (borrowed);
        expect_same_header (native.value (), owned.value ());
        expect_same_header (borrowed.value (), owned.value ());
        std::printf ("L2_BASELINE trailing-NUL accepted flow=%d\n", capture_flow);
    }
}

TEST (EnvelopeL2Wire, DirectCanonicalAndFallbackPreserveValuesErrorsAndRejectionContext)
{
    msg::envelope_codec_t codec;
    auto full = l2_wire_fixture ();
    full["correlationId"] = "id";
    full["deadline"] = "2026-11-14T00:00:00Z";
    full["topic"] = "topic-é";
    full["source"] = "node";
    full["errorCode"] = "internal_failure";
    full["errorMessage"] = "line\nend";
    full["metadata"] = {{"a", "first"}, {"z", "last"}};
    full["flowId"] = "019a55d5-0000-7000-8000-000000000001";
    full["flowOrigin"] = 3;
    std::vector<std::string> inputs{l2_wire_fixture ().dump (), full.dump (),
                                    node_ordered_header (full.dump ())};
    for (const auto *field : {"kind", "channelName", "messageName", "contentType", "correlationId",
                              "deadline", "topic", "errorCode", "errorMessage", "source", "flowId",
                              "flowOrigin", "formatMarker", "metadata"}) {
        for (const auto &value :
             {nlohmann::json (true), nlohmann::json (1.5), nlohmann::json (2147483648ULL),
              nlohmann::json::array (), nlohmann::json::object (), nlohmann::json (nullptr)}) {
            auto changed = full;
            changed[field] = value;
            inputs.push_back (changed.dump ());
        }
        auto missing = full;
        missing.erase (field);
        inputs.push_back (missing.dump ());
    }
    for (const auto *field : {"kind", "formatMarker", "flowOrigin"}) {
        for (const auto value : {-2147483648LL, -1LL, 0LL, 6LL, 2147483647LL, 2147483648LL}) {
            auto changed = full;
            changed[field] = value;
            inputs.push_back (changed.dump ());
        }
    }
    for (const auto *value : {"", "not-a-uuid"}) {
        auto changed = full;
        changed["flowId"] = value;
        inputs.push_back (changed.dump ());
    }
    for (const auto &metadata :
         {nlohmann::json{{"", "value"}}, nlohmann::json{{"key", std::string ("v\0", 2)}},
          nlohmann::json{{"key", std::string (4096, 'v')}}, nlohmann::json{{"key", 1}}}) {
        auto changed = full;
        changed["metadata"] = metadata;
        inputs.push_back (changed.dump ());
    }
    for (const auto &bad : {std::string ("\\q"), std::string ("\x80", 1)}) {
        auto changed = full;
        changed["channelName"] = "SENTINEL";
        auto wire = changed.dump ();
        wire.replace (wire.find ("SENTINEL"), 8, bad);
        inputs.push_back (std::move (wire));
    }
    auto duplicate = full.dump ();
    duplicate.insert (duplicate.find (",\"messageName\":"), ",\"kind\":1");
    inputs.push_back (duplicate);
    duplicate = full.dump ();
    duplicate.replace (duplicate.find ("\"a\":\"first\""), 11, "\"a\":\"first\",\"a\":\"second\"");
    inputs.push_back (duplicate);
    for (const bool capture_flow : {false, true}) {
        for (std::size_t index = 0; index < inputs.size (); ++index) {
            SCOPED_TRACE (std::to_string (index) + " flow=" + std::to_string (capture_flow));
            // A trailing space is legal JSON but never canonical. Keeping all
            // preceding bytes fixes the parser's diagnostic byte positions.
            const auto canonical = zlink::message_t::from (inputs[index]);
            const auto fallback = zlink::message_t::from (inputs[index] + " ");
            std::optional<msg::envelope_header_t> expected_context;
            const auto expected = codec.decode_header (canonical, capture_flow, &expected_context);
            for (const auto *wire : {&canonical, &fallback}) {
                for (const bool borrow : {false, true}) {
                    std::optional<msg::envelope_header_t> context;
                    const auto actual = borrow
                                          ? codec.decode_header_view (*wire, capture_flow, &context)
                                          : codec.decode_header (*wire, capture_flow, &context);
                    ASSERT_EQ (static_cast<bool> (actual), static_cast<bool> (expected));
                    ASSERT_EQ (context.has_value (), expected_context.has_value ());
                    if (context)
                        expect_same_header (*context, *expected_context);
                    if (actual) {
                        expect_same_header (actual.value (), expected.value ());
                    } else {
                        EXPECT_EQ (actual.error_kind (), expected.error_kind ());
                        EXPECT_STREQ (actual.error ()->what (), expected.error ()->what ());
                    }
                }
            }
            std::printf ("DIRECT_DIAG %zu %d %s\n", index, capture_flow,
                         expected ? "accepted" : expected.error ()->what ());
            const auto snapshot = [&] (const msg::envelope_header_t &header) {
                nlohmann::json values{{"kind", static_cast<int> (header.kind)},
                                      {"channelName", std::string (header.channel_name)},
                                      {"messageName", std::string (header.message_name)},
                                      {"contentType", std::string (header.content_type)},
                                      {"correlationId", std::string (header.correlation_id)},
                                      {"metadata", header.metadata}};
                values["deadline"] = header.deadline
                                       ? nlohmann::json (std::string (*header.deadline))
                                       : nlohmann::json (nullptr);
                const auto optional = [] (const std::optional<std::string> &value) {
                    return value ? nlohmann::json (*value) : nlohmann::json (nullptr);
                };
                values["topic"] = optional (header.topic);
                values["errorCode"] = optional (header.error_code);
                values["errorMessage"] = optional (header.error_message);
                values["source"] = optional (header.source);
                values["flowId"] = optional (header.flow_id);
                values["flowOrigin"] = header.flow_origin
                                         ? nlohmann::json (static_cast<int> (*header.flow_origin))
                                         : nlohmann::json (nullptr);
                return values.dump ();
            };
            std::printf ("DIRECT_VALUE %zu %d %s\n", index, capture_flow,
                         expected ? snapshot (expected.value ()).c_str () : "rejected");
            std::printf ("DIRECT_CONTEXT %zu %d %s\n", index, capture_flow,
                         expected_context ? snapshot (*expected_context).c_str () : "absent");
        }
    }
}

TEST (EnvelopeL2Wire, CanonicalAndFallbackRejectWithFrozenKindAndMessage)
{
    msg::envelope_codec_t codec;
    const auto prefix = std::string ("invalid ZLink envelope header: ");
    std::vector<std::pair<std::string, std::string>> cases;
    const auto add_field = [&] (const char *field, nlohmann::json value, const char *reason) {
        auto header = l2_wire_fixture ();
        header[field] = std::move (value);
        cases.emplace_back (header.dump (), prefix + reason);
    };
    add_field ("kind", true, "ZLink envelope header has invalid required fields");
    add_field ("kind", 2147483648ULL, "ZLink envelope header has invalid required fields");
    add_field ("kind", 0, "ZLink envelope header kind is invalid");
    add_field ("contentType", false, "ZLink envelope header contentType is invalid");
    add_field ("deadline", false, "ZLink envelope header has an invalid field type");
    add_field ("flowId", false, "ZLink envelope header flow fields are invalid");
    add_field ("metadata", false, "ZLink envelope header metadata must be an object");
    add_field ("metadata", {{"key", false}}, "ZLink envelope header metadata value is invalid");
    for (const auto *field : {"kind", "channelName", "messageName"}) {
        auto header = l2_wire_fixture ();
        header.erase (field);
        cases.emplace_back (header.dump (),
                            prefix + "ZLink envelope header has invalid required fields");
    }
    auto duplicate = l2_wire_fixture ().dump ();
    duplicate.insert (duplicate.find (",\"messageName\":"), ",\"kind\":1");
    cases.emplace_back (duplicate, prefix + "duplicate envelope header field");
    auto metadata = l2_wire_fixture ();
    metadata["metadata"] = {{"key", "value"}};
    duplicate = metadata.dump ();
    duplicate.insert (duplicate.find ("\"key\":"), "\"key\":\"first\",");
    cases.emplace_back (duplicate, prefix + "duplicate envelope metadata key");
    for (const auto &bad : {std::string ("\\q"), std::string ("\x80", 1)}) {
        auto header = l2_wire_fixture ();
        header["channelName"] = "SENTINEL";
        auto input = header.dump ();
        input.replace (input.find ("SENTINEL"), 8, bad);
        try {
            const auto parsed = nlohmann::json::parse (input);
            FAIL () << "malformed fixture was accepted by the JSON reference parser: "
                    << parsed.dump ();
        }
        catch (const nlohmann::json::parse_error &error) {
            cases.emplace_back (input, prefix + error.what ());
        }
    }
    for (const auto &[input, reason] : cases) {
        for (const auto &bytes : {input, input + " "}) {
            const auto wire = zlink::message_t::from (bytes);
            for (const bool borrow : {false, true}) {
                const auto decoded =
                  borrow ? codec.decode_header_view (wire, true) : codec.decode_header (wire, true);
                ASSERT_FALSE (decoded);
                EXPECT_EQ (decoded.error_kind (), fw::framework_error_kind_t::protocol_error);
                EXPECT_EQ (decoded.error ()->what (), reason);
            }
        }
    }
}

TEST (EnvelopeL2Wire, DirectDecodePreservesEveryFieldAndBorrowLifetime)
{
    msg::envelope_codec_t codec;
    msg::envelope_header_t expected{.kind = msg::message_kind_t::error,
                                    .channel_name = "a-channel-with-a-long-name",
                                    .message_name = "a-message-with-a-long-name",
                                    .content_type = "application/x-protobuf",
                                    .correlation_id = "correlation-id-with-a-long-value",
                                    .deadline = "2026-11-14T00:00:00Z",
                                    .topic = "topic-é",
                                    .error_code = "internal_failure",
                                    .error_message = "line\nend",
                                    .source = "node",
                                    .metadata = {{"a", "first"}, {"z", "last"}},
                                    .flow_id = "019a55d5-0000-7000-8000-000000000001",
                                    .flow_origin = fw::flow_origin_t::application};
    const auto bytes = codec.encode_header (expected).to_string ();
    for (const bool capture_flow : {true, false}) {
        if (!capture_flow) {
            expected.flow_id.reset ();
            expected.flow_origin.reset ();
        }
        for (const auto &input : {bytes, bytes + " ", node_ordered_header (bytes)}) {
            std::optional<msg::envelope_header_t> copy;
            {
                const auto wire = zlink::message_t::from (input);
                for (const bool borrow : {false, true}) {
                    const auto decoded = borrow ? codec.decode_header_view (wire, capture_flow)
                                                : codec.decode_header (wire, capture_flow);
                    ASSERT_TRUE (decoded);
                    expect_same_header (decoded.value (), expected);
                    const auto offset = bytes.find ("a-channel-with-a-long-name");
                    if (borrow && input == bytes)
                        EXPECT_EQ (decoded.value ().channel_name.data (),
                                   reinterpret_cast<const char *> (wire.data ()) + offset);
                    else
                        EXPECT_NE (decoded.value ().channel_name.data (),
                                   reinterpret_cast<const char *> (wire.data ()) + offset);
                    copy = decoded.value ();
                }
            }
            expect_same_header (*copy, expected);
        }
    }
}

} // namespace
