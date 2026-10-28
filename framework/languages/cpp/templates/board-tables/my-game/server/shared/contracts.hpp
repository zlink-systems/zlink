#pragma once
#include <zlink/json.hpp>
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
    static constexpr int seats = 4, deck_size = 52, ranks = 13, lowest_rank = 2,
                         cards_per_hand = 2, no_seat = -1, minimum_players = 2;
    static constexpr std::int64_t initial_chips = 1000;
    static constexpr std::int64_t max_contract_integer = 9'007'199'254'740'991LL;
    static constexpr auto table_full = "table-full";
    static constexpr auto no_hand = "no-hand";
    static constexpr auto hand_in_progress = "hand-in-progress";
    static constexpr auto not_enough_players = "not-enough-players";
    static constexpr auto not_your_turn = "not-your-turn";
    static constexpr auto invalid_amount = "invalid-amount";
    static constexpr auto not_seated = "not-seated";
    static constexpr auto integer_out_of_range = "integer-out-of-range";
    static constexpr auto packet_not_allowed = "packet-not-allowed";

    static constexpr bool is_contract_integer (std::int64_t value) noexcept
    {
        return value >= 0 && value <= max_contract_integer;
    }
    static constexpr bool try_add_contract_integers (std::int64_t left, std::int64_t right,
                                                     std::int64_t &sum) noexcept
    {
        if (!is_contract_integer (left) || !is_contract_integer (right)
            || right > max_contract_integer - left) {
            sum = 0;
            return false;
        }
        sum = left + right;
        return true;
    }
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
struct SessionInfo
{
    static constexpr auto packet_name = "SessionInfo";
    std::string sessionId, actorId;
    std::uint64_t objectGeneration = 0;
    bool operator== (const SessionInfo &) const = default;
};
struct JoinTableRequest
{
    static constexpr auto packet_name = "JoinTableRequest";
    std::string playerId, tableId;
};
struct BetRequest
{
    static constexpr auto packet_name = "BetRequest";
    std::int64_t handNumber = 0, amount = 0;
};
struct SeatView
{
    int seat = Rules::no_seat;
    std::string playerId;
    std::int64_t chips = 0;
    bool acted = false;
    int cardCount = 0;
    bool operator== (const SeatView &) const = default;
};
struct TableView
{
    static constexpr auto packet_name = "TableView";
    std::string tableId, nodeId;
    int yourSeat = Rules::no_seat;
    std::int64_t handNumber = 0;
    bool inHand = false;
    int turnSeat = Rules::no_seat;
    std::int64_t pot = 0;
    std::vector<SeatView> seats;

};
struct HandDealt
{
    static constexpr auto packet_name = "HandDealt";
    std::int64_t handNumber = 0;
    std::vector<int> cards;
    bool operator== (const HandDealt &) const = default;
};
struct RevealedHand
{
    int seat = Rules::no_seat;
    std::string playerId;
    std::vector<int> cards;
    bool operator== (const RevealedHand &) const = default;
};
struct HandResult
{
    static constexpr auto packet_name = "HandResult";
    std::int64_t handNumber = 0, pot = 0;
    std::vector<int> winners;
    std::vector<RevealedHand> hands;
    std::vector<SeatView> seats;
    bool operator== (const HandResult &) const = default;
};
struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string reason;
    std::int32_t retryAfterMs = 0;
};
}

namespace zlink::json
{
template <> struct dto_traits<::MyGame::SessionInfo>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("sessionId", &::MyGame::SessionInfo::sessionId),
      zlink::json::field ("actorId", &::MyGame::SessionInfo::actorId),
      zlink::json::field ("objectGeneration", &::MyGame::SessionInfo::objectGeneration)
    };
};

template <> struct dto_traits<::MyGame::JoinTableRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::JoinTableRequest::playerId),
      zlink::json::field ("tableId", &::MyGame::JoinTableRequest::tableId)
    };
};

template <> struct dto_traits<::MyGame::BetRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("handNumber", &::MyGame::BetRequest::handNumber),
      zlink::json::field ("amount", &::MyGame::BetRequest::amount)
    };
};

template <> struct dto_traits<::MyGame::SeatView>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("seat", &::MyGame::SeatView::seat),
      zlink::json::field ("playerId", &::MyGame::SeatView::playerId),
      zlink::json::field ("chips", &::MyGame::SeatView::chips),
      zlink::json::field ("acted", &::MyGame::SeatView::acted),
      zlink::json::field ("cardCount", &::MyGame::SeatView::cardCount)
    };
};

template <> struct dto_traits<::MyGame::TableView>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("tableId", &::MyGame::TableView::tableId),
      zlink::json::field ("nodeId", &::MyGame::TableView::nodeId),
      zlink::json::field ("yourSeat", &::MyGame::TableView::yourSeat),
      zlink::json::field ("handNumber", &::MyGame::TableView::handNumber),
      zlink::json::field ("inHand", &::MyGame::TableView::inHand),
      zlink::json::field ("turnSeat", &::MyGame::TableView::turnSeat),
      zlink::json::field ("pot", &::MyGame::TableView::pot),
      zlink::json::field ("seats", &::MyGame::TableView::seats)
    };
};

template <> struct dto_traits<::MyGame::HandDealt>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("handNumber", &::MyGame::HandDealt::handNumber),
      zlink::json::field ("cards", &::MyGame::HandDealt::cards)
    };
};

template <> struct dto_traits<::MyGame::RevealedHand>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("seat", &::MyGame::RevealedHand::seat),
      zlink::json::field ("playerId", &::MyGame::RevealedHand::playerId),
      zlink::json::field ("cards", &::MyGame::RevealedHand::cards)
    };
};

template <> struct dto_traits<::MyGame::HandResult>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("handNumber", &::MyGame::HandResult::handNumber),
      zlink::json::field ("pot", &::MyGame::HandResult::pot),
      zlink::json::field ("winners", &::MyGame::HandResult::winners),
      zlink::json::field ("hands", &::MyGame::HandResult::hands),
      zlink::json::field ("seats", &::MyGame::HandResult::seats)
    };
};

template <> struct dto_traits<::MyGame::ActionRejected>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("reason", &::MyGame::ActionRejected::reason),
      zlink::json::field ("retryAfterMs", &::MyGame::ActionRejected::retryAfterMs)
    };
};

template <> struct dto_traits<::MyGame::SessionInfoRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::TableViewRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::StartHandRequest>
{
    static constexpr auto fields = std::tuple{};
};
} // namespace zlink::json
