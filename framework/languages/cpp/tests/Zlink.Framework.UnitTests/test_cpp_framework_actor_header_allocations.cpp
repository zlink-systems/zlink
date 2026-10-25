/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <iostream>
#include <optional>

namespace
{
namespace fw = zlink::framework;
namespace detail = fw::detail;
namespace msg = fw::runtime::messaging;
using fw::tests::count_allocations;

msg::envelope_header_t input (bool request, bool metadata)
{
    msg::envelope_header_t header;
    header.kind = request ? msg::message_kind_t::request : msg::message_kind_t::command;
    header.message_name = std::string (128, 'p');
    header.correlation_id = std::string (64, 'c');
    header.flow_id = "01960000-0000-7000-8000-000000000001";
    header.flow_origin = fw::flow_origin_t::application;
    if (metadata)
        header.metadata.emplace (std::string (32, 'k'), std::string (128, 'v'));
    return header;
}

// Baseline uses the same STREAM codec as production, with its former owning
// header construction. It deliberately inherits improvements to that codec.
msg::message_parts_t owning_encode (const msg::envelope_header_t &header, zlink::message_t body)
{
    detail::stream_header_t wire (
      header.kind == msg::message_kind_t::command ? detail::stream_message_kind_t::send
                                                  : detail::stream_message_kind_t::request,
      detail::stream_codec_from_content_type (header.content_type),
      detail::stream_header_flags_t::none,
      header.kind == msg::message_kind_t::command ? std::nullopt : std::make_optional (1ULL),
      header.message_name, detail::stream_metadata_t (header.metadata));
    if (!header.correlation_id.empty ())
        wire.with_correlation_id (header.correlation_id);
    if (header.flow_id && header.flow_origin)
        wire.with_flow (*header.flow_id, *header.flow_origin);
    detail::stream_runtime_t::propagate_flow (wire);
    auto encoded = detail::stream_runtime_t (nullptr).encode_header (wire);
    return {zlink::message_t::from (encoded.value ()), std::move (body)};
}

TEST (ActorHeaderAllocations, EncoderOwnsOnlyTheFinalPartContainer)
{
    std::optional<msg::message_parts_t> result;
    const auto body = zlink::message_t::from (std::string ("{\"value\":7}"));
    const auto baseline =
      count_allocations ([&] { result.emplace (zlink::message_t{}, body.copy ()); });
    ASSERT_GT (baseline, 0u);
    for (const bool request : {false, true}) {
        for (const bool metadata : {false, true}) {
            const auto header = input (request, metadata);
            const auto expected = owning_encode (header, body.copy ());
            // Warm unrelated runtime initialization before observing new calls.
            result.emplace (detail::encode_actor_packet (header, body.copy ()));
            result.reset ();
            const auto measured = count_allocations (
              [&] { result.emplace (detail::encode_actor_packet (header, body.copy ())); });
            EXPECT_EQ (measured, baseline) << "request=" << request << " metadata=" << metadata;
            ASSERT_EQ (result->size (), 2u);
            EXPECT_EQ ((*result)[0].to_bytes (), expected[0].to_bytes ());
            EXPECT_EQ ((*result)[1].to_string (), body.to_string ());
            EXPECT_EQ (header.message_name, std::string (128, 'p'));
            EXPECT_EQ (header.correlation_id, std::string (64, 'c'));
            EXPECT_EQ (header.metadata.size (), metadata ? 1u : 0u);
            EXPECT_EQ (*header.flow_id, "01960000-0000-7000-8000-000000000001");
            result.reset ();
        }
    }
}

TEST (ActorHeaderAllocations, CanonicalRequestHeaderBytes)
{
    msg::envelope_header_t header;
    header.message_name = "p";
    header.correlation_id = "c";
    header.metadata.emplace ("k", "v");
    const auto actual = detail::encode_actor_packet (header, zlink::message_t{});
    // STREAM v2 JSON request: sequence 1, metadata k=v, correlation c.
    const std::vector<std::uint8_t> expected{0xf2, 2,   1, 0x0b, 0, 0, 0,   0, 0, 0,   0, 1,
                                             1,    'p', 0, 6,    1, 1, 'k', 0, 1, 'v', 1, 'c'};
    EXPECT_EQ (actual[0].to_bytes (), expected);
}

TEST (ActorHeaderAllocations, CounterDetectsTheOwningBaseline)
{
    auto header = input (true, true);
    const auto body = zlink::message_t::from (std::string ("{}"));
    std::optional<msg::message_parts_t> result;
    const auto baseline =
      count_allocations ([&] { result.emplace (zlink::message_t{}, body.copy ()); });
    result.reset ();
    const auto legacy =
      count_allocations ([&] { result.emplace (owning_encode (header, body.copy ())); });
    EXPECT_GT (legacy, baseline);
}

TEST (ActorHeaderAllocations, PreservesAmbientAndExplicitFlowSemantics)
{
    const auto body = zlink::message_t::from (std::string ("{}"));
    std::optional<msg::message_parts_t> observed;
    const auto baseline =
      count_allocations ([&] { observed.emplace (zlink::message_t{}, body.copy ()); });
    observed.reset ();
    for (const auto mode :
         {fw::message_flow_log_mode_t::off, fw::message_flow_log_mode_t::normal}) {
        fw::runtime::flow_context_t::scope_t scope (fw::runtime::flow_value_t{
          "01960000-0000-7000-8000-000000000002", fw::flow_origin_t::inbound, mode, std::nullopt});
        for (int variant = 0; variant != 5; ++variant) {
            auto header = input (true, false);
            header.message_name = "short";
            header.correlation_id.clear ();
            if (variant == 0) {
                header.flow_id.reset ();
                header.flow_origin.reset ();
            } else if (variant == 1) {
                header.flow_origin.reset ();
            } else if (variant == 2) {
                header.flow_id.reset ();
            } else if (variant == 3) {
                header.flow_id = "";
            }
            if (variant == 3 && mode == fw::message_flow_log_mode_t::off) {
                EXPECT_THROW (owning_encode (header, body.copy ()), fw::framework_exception_t);
                EXPECT_THROW (detail::encode_actor_packet (header, body.copy ()),
                              fw::framework_exception_t);
                continue;
            }
            const auto expected = owning_encode (header, body.copy ());
            const auto allocations = count_allocations (
              [&] { observed.emplace (detail::encode_actor_packet (header, body.copy ())); });
            EXPECT_EQ (allocations, baseline);
            EXPECT_EQ ((*observed)[0].to_bytes (), expected[0].to_bytes ());
            auto decoded =
              detail::stream_runtime_t::decode_header ((*observed)[0].to_bytes (), true);
            observed.reset ();
            ASSERT_TRUE (decoded);
            if (variant == 4) {
                ASSERT_TRUE (decoded.value ().flow_id ());
                EXPECT_EQ (*decoded.value ().flow_id (), "01960000-0000-7000-8000-000000000001");
            } else if (mode == fw::message_flow_log_mode_t::normal) {
                ASSERT_TRUE (decoded.value ().flow_id ());
                EXPECT_EQ (*decoded.value ().flow_id (), "01960000-0000-7000-8000-000000000002");
            } else {
                EXPECT_FALSE (decoded.value ().flow_id ());
            }
        }
    }
}

TEST (ActorHeaderAllocations, SharedValidationRejectsMalformedFields)
{
    const auto body = zlink::message_t::from (std::string ("{}"));
    for (int variant = 0; variant != 4; ++variant) {
        auto header = input (true, false);
        if (variant == 0)
            header.message_name = std::string (256, 'p');
        else if (variant == 1)
            header.correlation_id = std::string (256, 'c');
        else if (variant == 2)
            header.metadata.emplace ("", "value");
        else
            header.metadata.emplace ("key", std::string (65536, 'v'));
        EXPECT_THROW (owning_encode (header, body.copy ()), fw::framework_exception_t);
        EXPECT_THROW (detail::encode_actor_packet (header, body.copy ()),
                      fw::framework_exception_t);
    }
}

// The pre-change public-accessor body is the timing baseline; both variants
// remain indirect calls so the benchmark cannot inline away that baseline.
void original_accessor_flow (detail::stream_header_t &header)
{
    if (!header.flow_id () && header.kind () != detail::stream_message_kind_t::control) {
        if (const auto &flow = fw::runtime::flow_context_t::current ();
            flow && !flow->flow_id.empty ()
            && fw::runtime::flow_context_t::tracing (flow->diagnostics_mode))
            header.with_flow (flow->flow_id, flow->origin);
    }
}

TEST (ActorHeaderAllocations, OwningFlowAlternatingBenchmark)
{
    if (!std::getenv ("ZLINK_STREAM_FLOW_BENCH"))
        GTEST_SKIP () << "Set ZLINK_STREAM_FLOW_BENCH=1 for public STREAM flow timing";
    std::uint64_t checksum = 0;
    constexpr std::size_t iterations = 1000000;
    for (const bool ambient_off : {false, true}) {
        std::optional<fw::runtime::flow_value_t> ambient;
        if (ambient_off)
            ambient.emplace (fw::runtime::flow_value_t{
              "01960000-0000-7000-8000-000000000002", fw::flow_origin_t::inbound,
              fw::message_flow_log_mode_t::off, std::nullopt});
        fw::runtime::flow_context_t::scope_t scope (std::move (ambient));
        detail::stream_header_t header (detail::stream_message_kind_t::send,
                                        fw::stream_codec_t::json,
                                        detail::stream_header_flags_t::none, std::nullopt, "ping");
        const auto run = [&] (bool legacy, std::size_t count) {
            using propagation_t = void (*) (detail::stream_header_t &);
            propagation_t volatile propagate =
              legacy ? &original_accessor_flow : &detail::stream_runtime_t::propagate_flow;
            const auto start = std::chrono::steady_clock::now ();
            for (std::size_t index = 0; index < count; ++index)
                propagate (header);
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds> (
                                   std::chrono::steady_clock::now () - start)
                                   .count ();
            checksum += header.packet_name ().size () + count;
            return static_cast<double> (elapsed) / static_cast<double> (count);
        };
        (void) run (true, 20000);
        (void) run (false, 20000);
        std::array<double, 9> original{}, actual{};
        for (std::size_t pair = 0; pair < original.size (); ++pair) {
            if (pair % 2 == 0) {
                original[pair] = run (true, iterations);
                actual[pair] = run (false, iterations);
            } else {
                actual[pair] = run (false, iterations);
                original[pair] = run (true, iterations);
            }
            std::cout << "stream_flow_pair ambient_off=" << ambient_off << " pair=" << pair
                      << " A_ns=" << original[pair] << " B_ns=" << actual[pair] << '\n';
        }
        std::sort (original.begin (), original.end ());
        std::sort (actual.begin (), actual.end ());
        const auto original_allocations =
          count_allocations ([&] { original_accessor_flow (header); });
        const auto actual_allocations =
          count_allocations ([&] { detail::stream_runtime_t::propagate_flow (header); });
        EXPECT_EQ (original_allocations, 0u);
        EXPECT_EQ (actual_allocations, 0u);
        std::cout << "stream_flow_summary ambient_off=" << ambient_off << " A_ns=" << original[4]
                  << " B_ns=" << actual[4] << " B_over_A=" << actual[4] / original[4]
                  << " A_new=" << original_allocations << " B_new=" << actual_allocations << '\n';
        EXPECT_FALSE (header.flow_id ());
    }
    std::cout << "stream_flow_checksum=" << checksum << '\n';
    EXPECT_NE (checksum, 0u);
}

TEST (ActorHeaderAllocations, AlternatingBenchmark)
{
    if (!std::getenv ("ZLINK_ACTOR_HEADER_BENCH"))
        GTEST_SKIP () << "Set ZLINK_ACTOR_HEADER_BENCH=1 for the alternating microbenchmark";
    const auto body = zlink::message_t::from (std::string ("{\"value\":7}"));
    std::uint64_t checksum = 0;
    constexpr std::size_t iterations = 20000;
    for (unsigned cell = 0; cell != 6; ++cell) {
        const bool short_fields = cell >= 4;
        const bool request = short_fields ? cell == 5 : cell >= 2;
        const bool metadata = !short_fields && cell % 2 != 0;
        auto header = input (request, metadata);
        if (short_fields) {
            header.message_name = "ping";
            header.correlation_id.clear ();
            header.flow_id.reset ();
            header.flow_origin.reset ();
        }
        const auto run = [&] (bool legacy, std::size_t count) {
            const auto start = std::chrono::steady_clock::now ();
            for (std::size_t index = 0; index < count; ++index) {
                const auto parts = legacy ? owning_encode (header, body.copy ())
                                          : detail::encode_actor_packet (header, body.copy ());
                checksum += parts[0].size () + std::to_integer<unsigned> (parts[0].bytes ()[0]);
            }
            return static_cast<double> (std::chrono::duration_cast<std::chrono::nanoseconds> (
                                          std::chrono::steady_clock::now () - start)
                                          .count ())
                   / static_cast<double> (count);
        };
        (void) run (true, 2000);
        (void) run (false, 2000);
        std::array<double, 9> legacy{}, direct{};
        for (std::size_t pair = 0; pair < legacy.size (); ++pair) {
            if (pair % 2 == 0) {
                legacy[pair] = run (true, iterations);
                direct[pair] = run (false, iterations);
            } else {
                direct[pair] = run (false, iterations);
                legacy[pair] = run (true, iterations);
            }
            std::cout << "actor_header_pair request=" << request << " metadata=" << metadata
                      << " short=" << short_fields << " pair=" << pair << " A_ns=" << legacy[pair]
                      << " B_ns=" << direct[pair] << '\n';
        }
        std::sort (legacy.begin (), legacy.end ());
        std::sort (direct.begin (), direct.end ());
        std::optional<msg::message_parts_t> parts;
        const auto a_allocs =
          count_allocations ([&] { parts.emplace (owning_encode (header, body.copy ())); });
        parts.reset ();
        const auto b_allocs = count_allocations (
          [&] { parts.emplace (detail::encode_actor_packet (header, body.copy ())); });
        std::cout << "actor_header_summary request=" << request << " metadata=" << metadata
                  << " short=" << short_fields << " A_ns=" << legacy[4] << " B_ns=" << direct[4]
                  << " B_over_A=" << direct[4] / legacy[4] << " A_new=" << a_allocs
                  << " B_new=" << b_allocs << '\n';
    }
    std::cout << "actor_header_checksum=" << checksum << '\n';
    EXPECT_NE (checksum, 0u);
}
} // namespace

#include "../support/runtime_failure_fixture.hpp"
#include <future>
#include <thread>

namespace
{
#ifdef ZLINK_STREAM_RAW_BASELINE
using allocation_packet_t = zlink::message_t;
#else
using allocation_packet_t = fw::message_t;
#endif

enum class allocation_path_t
{
    reply,
    relay,
    relay_reply
};

class allocation_session_t final : public fw::packet_stream_session_t
{
  public:
    allocation_session_t (allocation_path_t path, fw::session_actor_t actor) :
        _path (path), _actor (std::move (actor))
    {
    }
    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }
    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &,
                                const allocation_packet_t &payload) override
    {
#ifndef ZLINK_STREAM_RAW_BASELINE
        if (!payload.encoded ())
            throw std::runtime_error ("received packet must remain encoded");
        std::optional<zlink::message_t> first, second;
        const auto copies = count_allocations ([&] {
            first.emplace (detail::message_to_raw (payload, _serializers));
            second.emplace (detail::message_to_raw (payload, _serializers));
        });
        if (copies != 0 || first->data () != second->data ())
            throw std::runtime_error ("encoded forwarding must share native payload storage");
#endif
        if (_path == allocation_path_t::reply) {
            co_await stream.reply_packet (payload).async ();
        } else if (_path == allocation_path_t::relay) {
            co_await _actor.relay (payload);
        } else {
            co_await stream.reply_packet (co_await _actor.relay_request (payload).async ())
              .async ();
        }
    }

  private:
    fw::serializer_registry_t _serializers;
    allocation_path_t _path;
    fw::session_actor_t _actor;
};

TEST (ActorHeaderAllocations, StreamEncodedForwardingDoesNotExceedRawMain)
{
    detail::configure_stream_dispatch_executor ();
    struct executor_guard_t
    {
        ~executor_guard_t () { detail::shutdown_stream_dispatch_executor (); }
    } guard;
    auto builder = fw::test::runtime_failure_builder ();
    builder.stream ("alloc").bind ("tcp://127.0.0.1:0").register_session ("session");
    fw::serializer_registry_t serializers;
    detail::bind_stream_serializers (builder, serializers);
    auto runtime = detail::stream_runtime_t::from (builder);
    auto stream = runtime.open_session ("alloc");
    detail::actor_gateway_runtime_t gateway;
    gateway.bind_serializers (serializers);
    auto actors = gateway.manager ();
    detail::session_actor_manager_access_t::attach (actors, stream);
    auto actor = actors
                   .bind (detail::actor_ref_access_t::make (fw::node_rid_t::from_string ("owner"),
                                                            "type", "actor", 1))
                   .async ()
                   .result ()
                   .value ();
    const auto body = zlink::message_t::from (std::string (4096, 'a'));
    std::size_t writes = 0, relays = 0;
    runtime.attach_transport_writer (
      stream,
      [&] (const detail::stream_header_t &header,
           const zlink::message_t &payload) -> fw::task_t<void> {
          ++writes;
          if (header.codec () != fw::stream_codec_t::raw || payload.size () != body.size ()
              || !std::equal (payload.bytes ().begin (), payload.bytes ().end (),
                              body.bytes ().begin ()))
              throw std::runtime_error ("encoded forwarding changed the packet");
          co_return;
      });
    gateway.on_relay (
      [&] (const fw::actor_ref_t &, fw::actor_context_t, const detail::stream_header_t &,
           const zlink::message_t &payload, std::optional<detail::bound_session_relay_source_t>,
           std::chrono::milliseconds) -> fw::request_task_t<std::optional<zlink::message_t>> {
          ++relays;
          co_await detail::request_admitted_t{};
          co_return std::optional<zlink::message_t> (payload.copy ());
      });
    const detail::stream_header_t header (
      detail::stream_message_kind_t::request, fw::stream_codec_t::raw,
      detail::stream_header_flags_t::has_request_seq, 1, "p", {});
    // Include every continuation thread of the same packet in the existing allocation counter.
    const auto measure = [&] (allocation_session_t &session) {
        std::promise<void> done;
        auto ready = done.get_future ();
        fw::result_t<void> result = fw::result_t<void>::success ();
        fw::tests::process_allocation_count.store (0);
        fw::tests::tracking_process_allocations.store (true);
        const auto submitted = runtime.dispatch_packet_async (
          session, stream, header, body, [&] (const fw::result_t<void> &completed) {
              result = completed;
              done.set_value ();
          });
        if (submitted)
            ready.get ();
        fw::tests::tracking_process_allocations.store (false);
        const auto allocations = fw::tests::process_allocation_count.load ();
        if (!submitted)
            throw fw::framework_exception_t (submitted.error_kind (), submitted.error ()->what ());
        if (!result)
            throw fw::framework_exception_t (result.error_kind (), result.error ()->what ());
        return allocations;
    };
    // Positive and negative probes prove that the counter detects allocation and the empty path.
    void *probe = nullptr;
    EXPECT_EQ (count_allocations ([&] { probe = ::operator new (64); }), 1u);
    ::operator delete (probe);
    EXPECT_EQ (count_allocations ([] {}), 0u);
    fw::tests::process_allocation_count.store (0);
    fw::tests::tracking_process_allocations.store (true);
    probe = ::operator new (64);
    fw::tests::tracking_process_allocations.store (false);
    ::operator delete (probe);
    EXPECT_EQ (fw::tests::process_allocation_count.load (), 1u);
    fw::tests::process_allocation_count.store (0);
    fw::tests::tracking_process_allocations.store (true);
    fw::tests::tracking_process_allocations.store (false);
    EXPECT_EQ (fw::tests::process_allocation_count.load (), 0u);
    const char *names[] = {"receive_reply", "receive_relay", "relay_reply"};
    for (int path = 0; path != 3; ++path) {
        allocation_session_t session (static_cast<allocation_path_t> (path), actor);
        for (int warm = 0; warm != 8; ++warm)
            (void) measure (session);
        auto measured = measure (session);
        auto minimum = measured;
        for (int packet = 0; packet != 31; ++packet) {
            const auto observed = measure (session);
            measured = std::max (measured, observed);
            minimum = std::min (minimum, observed);
        }
        std::printf ("STREAM allocations %s=%zu min=%zu packets=32\n", names[path], measured,
                     minimum);
#ifndef ZLINK_STREAM_RAW_BASELINE
        // GCC 13/libstdc++ Release, origin/main 3841c24541, the same 32-packet probe.
        constexpr std::size_t raw_main_maximum[] = {30, 78, 81};
        EXPECT_LE (measured, raw_main_maximum[path]) << names[path];
#endif
    }
    EXPECT_EQ (writes, 80u);
    EXPECT_EQ (relays, 80u);
}
} // namespace