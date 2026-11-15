/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#ifndef ZLINK_CPP_BENCH_GAME_MESSAGE_HPP
#define ZLINK_CPP_BENCH_GAME_MESSAGE_HPP

#include "bench_common.hpp"

#include <zlink/json.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace zlink_cpp_bench
{

struct game_item_t
{
    int32_t count = 0;
    std::string id;
    int32_t slot = 0;
};

struct game_message_t
{
    static constexpr const char *packet_name = "GameMessage";
    std::string display_name;
    int64_t gold = 987654321;
    int32_t heading = 271;
    int64_t hp = 287;
    std::vector<game_item_t> items;
    int64_t level = 42;
    int64_t mp = 164;
    bool online = true;
    int64_t phase = 0;
    std::string player_id;
    std::string room_id;
    int64_t run = 0;
    int64_t sent_ns = 0;
    int64_t seq = 0;
    std::string session;
    int32_t x = -123456;
    int32_t y = 7890;
    int32_t z = 654321;
};

void to_json (nlohmann::json &json, const game_item_t &value);
void from_json (const nlohmann::json &json, game_item_t &value);

inline size_t game_item_count (size_t payload_size)
{
    switch (payload_size) {
        case 1024:
            return 16;
        case 4096:
            return 88;
        default:
            throw std::invalid_argument ("zlink-cpp-json and zlink-framework-cpp-json support "
                                         "payload sizes 1024 and 4096 only");
    }
}

inline game_message_t make_game_message (
  size_t payload_size, uint32_t run, phase_t phase, uint64_t seq, uint64_t sent_ns = now_ns ())
{
    game_message_t value;
    value.display_name = "Player_12345678";
    value.phase = phase;
    value.player_id = "player-0000000000000001";
    value.room_id = "room-000000000000000001";
    value.run = run;
    value.sent_ns = static_cast<int64_t> (sent_ns);
    value.seq = static_cast<int64_t> (seq);
    value.session = "session-0000000000000001";
    const size_t count = game_item_count (payload_size);
    value.items.reserve (count);
    for (size_t i = 0; i < count; ++i) {
        const auto digits = std::to_string (i + 1);
        value.items.push_back (
          {static_cast<int32_t> (i % 9 + 1),
           "item-" + std::string (digits.size () < 8 ? 8 - digits.size () : 0, '0') + digits,
           static_cast<int32_t> (i % 32)});
    }
    return value;
}

inline bool game_stamp_matches (
  const game_message_t &value, uint32_t run, phase_t phase, uint64_t seq, size_t payload_size)
{
    return value.run == static_cast<int64_t> (run) && value.phase == static_cast<int64_t> (phase)
           && value.seq == static_cast<int64_t> (seq)
           && value.items.size () == game_item_count (payload_size);
}

inline nlohmann::json game_json (const game_message_t &value)
{
    return nlohmann::json{{"displayName", value.display_name},
                          {"gold", std::to_string (value.gold)},
                          {"heading", value.heading},
                          {"hp", std::to_string (value.hp)},
                          {"items", value.items},
                          {"level", std::to_string (value.level)},
                          {"mp", std::to_string (value.mp)},
                          {"online", value.online},
                          {"phase", std::to_string (value.phase)},
                          {"playerId", value.player_id},
                          {"roomId", value.room_id},
                          {"run", std::to_string (value.run)},
                          {"sentNs", std::to_string (value.sent_ns)},
                          {"seq", std::to_string (value.seq)},
                          {"session", value.session},
                          {"x", value.x},
                          {"y", value.y},
                          {"z", value.z}};
}

inline void to_json (nlohmann::json &json, const game_item_t &value)
{
    json = nlohmann::json{{"count", value.count}, {"id", value.id}, {"slot", value.slot}};
}

inline void from_json (const nlohmann::json &json, game_item_t &value)
{
    json.at ("count").get_to (value.count);
    json.at ("id").get_to (value.id);
    json.at ("slot").get_to (value.slot);
}

inline void to_json (nlohmann::json &json, const game_message_t &value)
{
    json = game_json (value);
}

inline void from_json (const nlohmann::json &json, game_message_t &value)
{
    value.display_name = json.at ("displayName").get<std::string> ();
    value.gold = std::stoll (json.at ("gold").get<std::string> ());
    value.heading = json.at ("heading").get<int32_t> ();
    value.hp = std::stoll (json.at ("hp").get<std::string> ());
    value.items = json.at ("items").get<std::vector<game_item_t>> ();
    value.level = std::stoll (json.at ("level").get<std::string> ());
    value.mp = std::stoll (json.at ("mp").get<std::string> ());
    value.online = json.at ("online").get<bool> ();
    value.phase = std::stoll (json.at ("phase").get<std::string> ());
    value.player_id = json.at ("playerId").get<std::string> ();
    value.room_id = json.at ("roomId").get<std::string> ();
    value.run = std::stoll (json.at ("run").get<std::string> ());
    value.sent_ns = std::stoll (json.at ("sentNs").get<std::string> ());
    value.seq = std::stoll (json.at ("seq").get<std::string> ());
    value.session = json.at ("session").get<std::string> ();
    value.x = json.at ("x").get<int32_t> ();
    value.y = json.at ("y").get<int32_t> ();
    value.z = json.at ("z").get<int32_t> ();
}

inline std::string encode_game_json (const game_message_t &value)
{
    return game_json (value).dump ();
}

inline game_message_t decode_game_json (const void *data, size_t size)
{
    return nlohmann::json::parse (static_cast<const char *> (data),
                                  static_cast<const char *> (data) + size)
      .get<game_message_t> ();
}

} // namespace zlink_cpp_bench

namespace zlink::json
{
template <> struct dto_traits<zlink_cpp_bench::game_item_t>
{
    static constexpr auto fields = std::tuple{field ("count", &zlink_cpp_bench::game_item_t::count),
                                              field ("id", &zlink_cpp_bench::game_item_t::id),
                                              field ("slot", &zlink_cpp_bench::game_item_t::slot)};
};
template <> struct dto_traits<zlink_cpp_bench::game_message_t>
{
    static constexpr auto fields =
      std::tuple{field ("displayName", &zlink_cpp_bench::game_message_t::display_name),
                 field ("gold", &zlink_cpp_bench::game_message_t::gold),
                 field ("heading", &zlink_cpp_bench::game_message_t::heading),
                 field ("hp", &zlink_cpp_bench::game_message_t::hp),
                 field ("items", &zlink_cpp_bench::game_message_t::items),
                 field ("level", &zlink_cpp_bench::game_message_t::level),
                 field ("mp", &zlink_cpp_bench::game_message_t::mp),
                 field ("online", &zlink_cpp_bench::game_message_t::online),
                 field ("phase", &zlink_cpp_bench::game_message_t::phase),
                 field ("playerId", &zlink_cpp_bench::game_message_t::player_id),
                 field ("roomId", &zlink_cpp_bench::game_message_t::room_id),
                 field ("run", &zlink_cpp_bench::game_message_t::run),
                 field ("sentNs", &zlink_cpp_bench::game_message_t::sent_ns),
                 field ("seq", &zlink_cpp_bench::game_message_t::seq),
                 field ("session", &zlink_cpp_bench::game_message_t::session),
                 field ("x", &zlink_cpp_bench::game_message_t::x),
                 field ("y", &zlink_cpp_bench::game_message_t::y),
                 field ("z", &zlink_cpp_bench::game_message_t::z)};
};
} // namespace zlink::json

#endif
