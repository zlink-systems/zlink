#pragma once
#include "server/shared/contracts.hpp"
#include <cerrno>
#include <cstdint>
#include <functional>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <bcrypt.h>
#elif defined(__linux__)
#include <sys/random.h>
#else
#error "board-tables requires Linux getrandom or Windows BCryptGenRandom"
#endif

namespace MyGame
{
struct Seat
{
    std::string playerId;
    std::int64_t chips = Rules::initial_chips;
    std::vector<int> cards;
    bool acted = false;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (Seat, playerId, chips, cards, acted)
};
class TableState
{
  public:
    using RandomWordSource = std::function<std::uint32_t ()>;

  private:
    static std::uint32_t system_random_word ()
    {
        std::uint32_t value = 0;
#ifdef _WIN32
        const auto status = BCryptGenRandom (nullptr, reinterpret_cast<PUCHAR> (&value),
                                             sizeof (value), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0)
            throw std::runtime_error ("BCryptGenRandom failed");
#else
        auto *bytes = reinterpret_cast<unsigned char *> (&value);
        std::size_t filled = 0;
        while (filled < sizeof (value)) {
            const auto count = ::getrandom (bytes + filled, sizeof (value) - filled, 0);
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                throw std::system_error (errno, std::generic_category (), "getrandom failed");
            }
            if (count == 0)
                throw std::runtime_error ("getrandom returned no data");
            filled += static_cast<std::size_t> (count);
        }
#endif
        return value;
    }
    std::uint32_t random_below (std::uint32_t bound)
    {
        if (bound == 0)
            throw std::invalid_argument ("Random bound must be positive");
        const auto threshold = static_cast<std::uint32_t> (-bound) % bound;
        while (true) {
            const auto value = random_word_source_ ();
            if (value >= threshold)
                return value % bound;
        }
    }
    RandomWordSource random_word_source_;

  public:
    std::vector<Seat> seats;
    std::vector<int> deck;
    std::int64_t handNumber = 0, pot = 0;
    int turnSeat = Rules::no_seat;
    bool inHand = false;

    TableState () : random_word_source_ (&TableState::system_random_word) {}
    explicit TableState (RandomWordSource random_word_source) :
        random_word_source_ (std::move (random_word_source))
    {
        if (!random_word_source_)
            throw std::invalid_argument ("Random word source must be callable");
    }

    NLOHMANN_DEFINE_TYPE_INTRUSIVE (TableState, seats, deck, handNumber, pot, turnSeat, inHand)
    int seat_of (std::string_view id) const
    {
        for (std::size_t i = 0; i < seats.size (); ++i)
            if (seats[i].playerId == id)
                return static_cast<int> (i);
        return Rules::no_seat;
    }
    // Seats the player at admission; a seated player is admitted again without a new seat.
    bool admit (std::string_view id)
    {
        if (seat_of (id) != Rules::no_seat)
            return true;
        if (seats.size () == Rules::seats)
            return false;
        seats.push_back (Seat{std::string (id), Rules::initial_chips, {}, false});
        return true;
    }
    std::vector<SeatView> views () const
    {
        std::vector<SeatView> result;
        result.reserve (seats.size ());
        for (std::size_t i = 0; i < seats.size (); ++i) {
            const auto &s = seats[i];
            result.push_back ({static_cast<int> (i), s.playerId, s.chips, s.acted,
                               static_cast<int> (s.cards.size ())});
        }
        return result;
    }
    TableView
    view (std::string_view player, const std::string &table, const std::string &node) const
    {
        return {table, node, seat_of (player), handNumber, inHand, turnSeat, pot, views ()};
    }
    std::string start (std::string_view id)
    {
        if (seat_of (id) == Rules::no_seat)
            return Rules::not_seated;
        if (inHand)
            return Rules::hand_in_progress;
        if (seats.size () < Rules::minimum_players)
            return Rules::not_enough_players;
        std::int64_t next_hand_number = 0;
        if (!Rules::try_add_contract_integers (handNumber, 1, next_hand_number))
            return Rules::integer_out_of_range;
        deck.resize (Rules::deck_size);
        std::iota (deck.begin (), deck.end (), 0);
        for (std::size_t i = deck.size (); i > 1; --i) {
            const auto selected = random_below (static_cast<std::uint32_t> (i));
            std::swap (deck[i - 1], deck[selected]);
        }
        for (auto &seat : seats) {
            seat.cards.clear ();
            seat.acted = false;
            for (int i = 0; i < Rules::cards_per_hand; ++i) {
                seat.cards.push_back (deck.back ());
                deck.pop_back ();
            }
        }
        handNumber = next_hand_number;
        pot = 0;
        turnSeat = 0;
        inHand = true;
        return {};
    }
    struct BetOutcome
    {
        std::string rejection;
        std::optional<HandResult> result;
    };
    BetOutcome bet (std::string_view id, const BetRequest &request)
    {
        const auto index = seat_of (id);
        if (index == Rules::no_seat)
            return {Rules::not_seated, {}};
        if (request.handNumber > Rules::max_contract_integer
            || request.amount > Rules::max_contract_integer)
            return {Rules::integer_out_of_range, {}};
        if (!inHand || request.handNumber != handNumber)
            return {Rules::no_hand, {}};
        if (index != turnSeat)
            return {Rules::not_your_turn, {}};
        auto &seat = seats[index];
        if (request.amount < 0 || request.amount > seat.chips)
            return {Rules::invalid_amount, {}};
        std::int64_t next_pot = 0;
        if (!Rules::try_add_contract_integers (pot, request.amount, next_pot))
            return {Rules::integer_out_of_range, {}};
        int next_turn = Rules::no_seat;
        for (std::size_t i = index + 1; i < seats.size (); ++i)
            if (!seats[i].cards.empty () && !seats[i].acted) {
                next_turn = static_cast<int> (i);
                break;
            }
        if (next_turn != Rules::no_seat) {
            seat.chips -= request.amount;
            pot = next_pot;
            seat.acted = true;
            turnSeat = next_turn;
            return {};
        }
        HandResult result;
        result.handNumber = handNumber;
        result.pot = next_pot;
        int best = -1;
        for (std::size_t i = 0; i < seats.size (); ++i) {
            const auto &s = seats[i];
            if (s.cards.empty ())
                continue;
            result.hands.push_back ({static_cast<int> (i), s.playerId, s.cards});
            int score = 0;
            for (int card : s.cards)
                score += card % Rules::ranks + Rules::lowest_rank;
            if (score > best) {
                best = score;
                result.winners.clear ();
            }
            if (score == best)
                result.winners.push_back (static_cast<int> (i));
        }
        const auto winner_count = static_cast<std::int64_t> (result.winners.size ());
        const auto share = next_pot / winner_count;
        const auto remainder = next_pot % winner_count;
        std::vector<std::int64_t> winner_balances;
        winner_balances.reserve (result.winners.size ());
        for (std::size_t i = 0; i < result.winners.size (); ++i) {
            const auto winner = result.winners[i];
            auto balance = seats[winner].chips;
            if (winner == index)
                balance -= request.amount;
            if (!Rules::try_add_contract_integers (balance, share, balance))
                return {Rules::integer_out_of_range, {}};
            if (i == 0 && !Rules::try_add_contract_integers (balance, remainder, balance))
                return {Rules::integer_out_of_range, {}};
            winner_balances.push_back (balance);
        }
        seat.chips -= request.amount;
        pot = next_pot;
        seat.acted = true;
        inHand = false;
        turnSeat = Rules::no_seat;
        pot = 0;
        for (std::size_t i = 0; i < result.winners.size (); ++i)
            seats[result.winners[i]].chips = winner_balances[i];
        for (auto &player : seats)
            player.cards.clear ();
        result.seats = views ();
        return {{}, std::move (result)};
    }
};
}
