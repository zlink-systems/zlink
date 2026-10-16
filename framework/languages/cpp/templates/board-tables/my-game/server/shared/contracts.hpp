#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace MyGame
{
struct Names
{
    static constexpr auto mesh = "my-game-tables";
    static constexpr auto player = "Player";
    static constexpr auto table = "Table";
    static constexpr auto stream = "players";
};
struct Rules
{
    static constexpr int seats = 4, initial_chips = 1000, deck_size = 52, ranks = 13,
                         lowest_rank = 2, cards_per_hand = 2, no_seat = -1, minimum_players = 2;
    static constexpr auto table_full = "table-full";
    static constexpr auto no_hand = "no-hand";
    static constexpr auto hand_in_progress = "hand-in-progress";
    static constexpr auto not_enough_players = "not-enough-players";
    static constexpr auto not_your_turn = "not-your-turn";
    static constexpr auto invalid_amount = "invalid-amount";
    static constexpr auto not_seated = "not-seated";
};
struct Limits
{
    static constexpr auto packet = std::chrono::seconds (10);
    static constexpr auto startup = std::chrono::seconds (30);
    static constexpr auto relocation = std::chrono::seconds (30);
    static constexpr auto message_follow = std::chrono::seconds (30);
    static constexpr auto safe = message_follow + relocation;
    static constexpr auto shutdown = std::chrono::seconds (30);
    static constexpr std::size_t observation_capacity = 128;
};
struct Control
{
    static constexpr auto ready = "READY ";
    static constexpr auto drain = "drain";
    static constexpr auto shutdown = "shutdown";
    static constexpr auto requested = "RELOCATION_REQUESTED";
    static constexpr auto relocated = "RELOCATED";
    static constexpr auto safe = "SAFE_TO_SHUTDOWN";
    static constexpr auto stopped = "STOPPED";
};
struct SessionInfoRequest
{
    static constexpr auto packet_name = "SessionInfoRequest";
};
struct TableViewRequest
{
    static constexpr auto packet_name = "TableViewRequest";
};
struct StartHandRequest
{
    static constexpr auto packet_name = "StartHandRequest";
};
template <class T>
requires (std::is_same_v<T, SessionInfoRequest> || std::is_same_v<T, TableViewRequest>
          || std::is_same_v<T, StartHandRequest>) void to_json (nlohmann::json &j, const T &)
{
    j = nlohmann::json::object ();
}
template <class T>
requires (std::is_same_v<T, SessionInfoRequest> || std::is_same_v<T, TableViewRequest>
          || std::is_same_v<T, StartHandRequest>) void from_json (const nlohmann::json &, T &)
{
}
struct SessionInfo
{
    static constexpr auto packet_name = "SessionInfo";
    std::string sessionId, actorId;
    std::uint64_t objectGeneration = 0;
    bool operator== (const SessionInfo &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (SessionInfo, sessionId, actorId, objectGeneration)
};
struct JoinTableRequest
{
    static constexpr auto packet_name = "JoinTableRequest";
    std::string playerId, tableId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinTableRequest, playerId, tableId)
};
struct BetRequest
{
    static constexpr auto packet_name = "BetRequest";
    int handNumber = 0, amount = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (BetRequest, handNumber, amount)
};
struct SeatView
{
    int seat = Rules::no_seat;
    std::string playerId;
    int chips = 0;
    bool acted = false;
    int cardCount = 0;
    bool operator== (const SeatView &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (SeatView, seat, playerId, chips, acted, cardCount)
};
struct TableView
{
    static constexpr auto packet_name = "TableView";
    std::string tableId, nodeId;
    int yourSeat = Rules::no_seat, handNumber = 0;
    bool inHand = false;
    int turnSeat = Rules::no_seat, pot = 0;
    std::vector<SeatView> seats;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (
      TableView, tableId, nodeId, yourSeat, handNumber, inHand, turnSeat, pot, seats)
};
struct HandDealt
{
    static constexpr auto packet_name = "HandDealt";
    int handNumber = 0;
    std::vector<int> cards;
    bool operator== (const HandDealt &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (HandDealt, handNumber, cards)
};
struct RevealedHand
{
    int seat = Rules::no_seat;
    std::string playerId;
    std::vector<int> cards;
    bool operator== (const RevealedHand &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (RevealedHand, seat, playerId, cards)
};
struct HandResult
{
    static constexpr auto packet_name = "HandResult";
    int handNumber = 0, pot = 0;
    std::vector<int> winners;
    std::vector<RevealedHand> hands;
    std::vector<SeatView> seats;
    bool operator== (const HandResult &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (HandResult, handNumber, pot, winners, hands, seats)
};
struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string reason;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ActionRejected, reason)
};
}
