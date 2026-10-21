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
