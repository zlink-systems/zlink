/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/json.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "allocation_counter.hpp"
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
    // The SAX metadata owner, final header and result moves are required
    // storage. MSVC allocates a head node even for an empty map. This also
    // rejects a discarded JSON default without assuming string capacity.
    const auto reference = [] {
        std::map<std::string, std::string> metadata;
        msg::envelope_header_t value{
          .channel_name = "c", .message_name = "m", .content_type = "x/y"};
        value.metadata = std::move (metadata);
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
    alignas (record_t) std::byte baseline_storage[sizeof (record_t)];
    alignas (record_t) std::byte actual_storage[sizeof (record_t)];
    record_t *baseline = nullptr;
    record_t *actual = nullptr;
    const auto container_allocations = count_allocations ([&] {
        baseline = ::new (static_cast<void *> (baseline_storage)) record_t{};
        baseline->envelope_header = {.content_type = {}};
    });
    const auto allocations =
      count_allocations ([&] { actual = ::new (static_cast<void *> (actual_storage)) record_t{}; });
    EXPECT_EQ (allocations, container_allocations);
    EXPECT_TRUE (baseline->parts ().empty ());
    EXPECT_TRUE (actual->parts ().empty ());
    std::destroy_at (baseline);
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
