/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "../common/bench_common.hpp"
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/json.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

namespace zlink_cpp_bench::ladder
{
enum class payload_shape_t { bytes, mixed };
inline constexpr std::uint64_t game_timestamp_offset = 1000000000000000000ULL;
inline std::uint64_t game_timestamp_now () { return game_timestamp_offset + now_ns (); }

inline payload_shape_t read_payload_shape ()
{
    const char *value = std::getenv ("ZLINK_CPP_LADDER_SHAPE");
    if (!value || std::string_view (value) == "bytes")
        return payload_shape_t::bytes;
    if (std::string_view (value) == "mixed")
        return payload_shape_t::mixed;
    throw std::invalid_argument ("ZLINK_CPP_LADDER_SHAPE must be bytes or mixed");
}

struct typed_payload_t
{
    static constexpr const char *packet_name = "BenchPayload";
    std::vector<std::byte> bytes;
    const auto &body () const noexcept { return bytes; }
    void set_body (const std::vector<std::byte> &value) { bytes = value; }
};

struct game_item_t
{
    std::string id;
    std::int32_t count = 0;
    std::int32_t slot = 0;
};

struct game_message_t
{
    static constexpr const char *packet_name = "BenchPayload";
    std::string player_id;
    std::string room_id;
    std::string session;
    std::string display_name;
    std::int64_t seq = 0;
    std::int64_t run = 0;
    std::int64_t phase = 0;
    std::int64_t hp = 287;
    std::int64_t mp = 164;
    std::int64_t level = 42;
    std::int64_t gold = 987654321;
    std::int32_t x = -123456;
    std::int32_t y = 7890;
    std::int32_t z = 654321;
    std::int32_t heading = 271;
    bool online = true;
    std::int64_t sent_ns = 1700000000000000000LL;
    std::vector<game_item_t> items;
};

}

namespace zlink::json
{
template <> struct dto_traits<zlink_cpp_bench::ladder::typed_payload_t>
{
    static constexpr auto fields = std::tuple{
      field ("body", &zlink_cpp_bench::ladder::typed_payload_t::bytes)};
};
template <> struct dto_traits<zlink_cpp_bench::ladder::game_item_t>
{
    static constexpr auto fields = std::tuple{
      field ("id", &zlink_cpp_bench::ladder::game_item_t::id),
      field ("count", &zlink_cpp_bench::ladder::game_item_t::count),
      field ("slot", &zlink_cpp_bench::ladder::game_item_t::slot)};
};
template <> struct dto_traits<zlink_cpp_bench::ladder::game_message_t>
{
    static constexpr auto fields = std::tuple{
      field ("displayName", &zlink_cpp_bench::ladder::game_message_t::display_name),
      field ("gold", &zlink_cpp_bench::ladder::game_message_t::gold),
      field ("heading", &zlink_cpp_bench::ladder::game_message_t::heading),
      field ("hp", &zlink_cpp_bench::ladder::game_message_t::hp),
      field ("items", &zlink_cpp_bench::ladder::game_message_t::items),
      field ("level", &zlink_cpp_bench::ladder::game_message_t::level),
      field ("mp", &zlink_cpp_bench::ladder::game_message_t::mp),
      field ("online", &zlink_cpp_bench::ladder::game_message_t::online),
      field ("phase", &zlink_cpp_bench::ladder::game_message_t::phase),
      field ("playerId", &zlink_cpp_bench::ladder::game_message_t::player_id),
      field ("roomId", &zlink_cpp_bench::ladder::game_message_t::room_id),
      field ("run", &zlink_cpp_bench::ladder::game_message_t::run),
      field ("sentNs", &zlink_cpp_bench::ladder::game_message_t::sent_ns),
      field ("seq", &zlink_cpp_bench::ladder::game_message_t::seq),
      field ("session", &zlink_cpp_bench::ladder::game_message_t::session),
      field ("x", &zlink_cpp_bench::ladder::game_message_t::x),
      field ("y", &zlink_cpp_bench::ladder::game_message_t::y),
      field ("z", &zlink_cpp_bench::ladder::game_message_t::z)};
};
}

namespace zlink_cpp_bench::ladder
{

inline typed_payload_t make_typed_payload (size_t size, uint32_t run, phase_t phase, uint64_t seq)
{
    typed_payload_t value;
    value.bytes.assign (size, std::byte{0xab});
    stamp_payload (value.bytes.data (), size, run, phase, seq);
    return value;
}

inline auto typed_serializer ()
{
    static const zlink::framework::serializer_registry_t registry;
    return registry.get<typed_payload_t> ();
}

inline auto game_serializer ()
{
    static const zlink::framework::serializer_registry_t registry;
    return registry.get<game_message_t> ();
}

inline game_message_t game_message (uint32_t run, phase_t phase, uint64_t seq,
                                    std::uint64_t sent_ns = game_timestamp_now (), std::size_t item_count = 0)
{
    game_message_t value;
    value.player_id = "player-0000000000000001";
    value.room_id = "room-000000000000000001";
    value.session = "session-0000000000000001";
    value.display_name = "Player_12345678";
    value.seq = static_cast<std::int64_t> (seq);
    value.run = run;
    value.phase = phase;
    value.sent_ns = static_cast<std::int64_t> (sent_ns);
    value.items.reserve (item_count);
    for (std::size_t i = 0; i < item_count; ++i) {
        auto digits = std::to_string (i + 1);
        auto id = "item-" + std::string (digits.size () < 8 ? 8 - digits.size () : 0, '0') + digits;
        value.items.push_back ({std::move (id), static_cast<std::int32_t> (i % 9 + 1),
                                static_cast<std::int32_t> (i % 32)});
    }
    return value;
}

inline std::size_t game_json_size (const game_message_t &value)
{
    return zlink::framework::detail::encoded_payload_to_raw (
      game_serializer ().serialize (value)).size ();
}

struct game_payload_plan_t { std::size_t item_count; std::size_t json_size; };

inline game_payload_plan_t compute_game_payload_plan (std::size_t target)
{
    std::size_t best_count = 0, best_size = 0;
    auto best_delta = std::numeric_limits<std::size_t>::max ();
    for (std::size_t count = 0; count < target; ++count) {
        auto candidate = game_message (123, phase_active, 456, 1700000000000000000ULL, count);
        const auto actual = game_json_size (candidate);
        const auto delta = actual > target ? actual - target : target - actual;
        if (delta < best_delta) { best_delta = delta; best_count = count; best_size = actual; }
        if (actual > target + target / 50)
            break;
    }
    if (best_delta * 100 > target * 2)
        throw std::runtime_error ("mixed JSON body cannot fit target within 2 percent");
    return {best_count, best_size};
}

inline game_payload_plan_t game_payload_plan (std::size_t target)
{
    static const auto one_k = compute_game_payload_plan (1024);
    static const auto four_k = compute_game_payload_plan (4096);
    if (target == 1024) return one_k;
    if (target == 4096) return four_k;
    return compute_game_payload_plan (target);
}

inline std::size_t game_wire_size (std::size_t target, std::uint32_t run, phase_t phase,
                                   std::uint64_t seq)
{
    const auto base = game_payload_plan (target).json_size;
    const auto digits = [] (std::uint64_t value) { return std::to_string (value).size (); };
    const auto delta = static_cast<std::int64_t> (digits (run)) - static_cast<std::int64_t> (digits (123))
      + static_cast<std::int64_t> (digits (phase)) - static_cast<std::int64_t> (digits (phase_active))
      + static_cast<std::int64_t> (digits (seq)) - static_cast<std::int64_t> (digits (456));
    return static_cast<std::size_t> (static_cast<std::int64_t> (base) + delta);
}

inline game_message_t make_game_message (std::size_t target, uint32_t run, phase_t phase,
                                         uint64_t seq, std::uint64_t sent_ns = game_timestamp_now ())
{
    const auto plan = game_payload_plan (target);
    auto value = game_message (run, phase, seq, sent_ns, plan.item_count);
    return value;
}
}
