#pragma once
#include "world.hpp"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
#include <zlink/stream_connector.hpp>

namespace MyGame::E2E
{
namespace sc = zlink::stream_connector;
template <class T> T checked (sc::result_t<T> result)
{
    require (static_cast<bool> (result),
             result.error () ? result.error ()->message : "Connector operation failed");
    return result.value ();
}
inline void checked (sc::result_t<void> result)
{
    require (static_cast<bool> (result),
             result.error () ? result.error ()->message : "Connector operation failed");
}
template <class Message>
class EventWaiter
{
  public:
    struct State
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::optional<Message> message;
    };

  private:
    std::shared_ptr<State> state_;
    sc::subscription_t subscription_;

  public:
    EventWaiter (std::shared_ptr<State> state, sc::subscription_t subscription) :
        state_ (std::move (state)), subscription_ (std::move (subscription))
    {
    }
    EventWaiter (EventWaiter &&) noexcept = default;
    EventWaiter &operator= (EventWaiter &&) noexcept = default;
    EventWaiter (const EventWaiter &) = delete;
    EventWaiter &operator= (const EventWaiter &) = delete;
    Message wait ()
    {
        std::unique_lock lock (state_->mutex);
        require (state_->changed.wait_for (lock, Limits::packet, [&] { return state_->message.has_value (); }),
                 "Expected packet was not received");
        return *state_->message;
    }
};

class Client
{
    struct MoveState
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::optional<std::variant<WorldState, ActionRejected>> result;
    };
    std::mutex error_mutex_;
    std::string error_;
    sc::connector_t connector_;
    sc::subscription_t error_subscription_;

  public:
    const std::string player_id;
    Client (const std::string &endpoint, std::string id) : player_id (std::move (id))
    {
        sc::connector_options_t options;
        options.endpoint = endpoint;
        options.dispatch_mode = sc::dispatch_mode_t::immediate;
        options.reconnect.enabled = false;
        options.wait_timeout = Limits::packet;
        connector_ = sc::connector_factory_t::create (options);
        error_subscription_ = connector_.on_error ([this] (const auto &error) {
            std::lock_guard lock (error_mutex_);
            if (error_.empty ())
                error_ = error.message;
        });
        checked (connector_.connect ());
    }
    void healthy ()
    {
        require (connector_.is_connected (), "Client disconnected");
        std::lock_guard lock (error_mutex_);
        require (error_.empty (), "Connector error: " + error_);
    }
    bool connected () { return connector_.is_connected (); }
    template <class Message, class Predicate>
    EventWaiter<Message> observe (Predicate matches)
    {
        using State = typename EventWaiter<Message>::State;
        auto state = std::make_shared<State> ();
        auto subscription = connector_.on<Message> ([state, matches = std::move (matches)] (const auto &message) {
            if (!matches (message.payload))
                return;
            {
                std::lock_guard lock (state->mutex);
                if (state->message)
                    return;
                state->message = message.payload;
            }
            state->changed.notify_one ();
        });
        return EventWaiter<Message> (std::move (state), std::move (subscription));
    }
    template <class Message>
    EventWaiter<Message> observe ()
    {
        return observe<Message> ([] (const Message &) { return true; });
    }
    template <class Request, class Reply, class Predicate = std::function<bool (const Reply &)>>
    Reply exchange (const Request &request, Predicate matches = [] (const Reply &) { return true; })
    {
        healthy ();
        auto response = observe<Reply> (std::move (matches));
        connector_.send (request).submit ();
        const auto reply = response.wait ();
        healthy ();
        return reply;
    }
    WorldState join (double x) { return exchange<JoinWorldRequest, WorldState> ({player_id, x}); }
    void send_move (double delta, std::int64_t sequence)
    {
        healthy ();
        connector_.send (MoveRequest{delta, sequence}).submit ();
    }
    std::variant<WorldState, ActionRejected> move_result (double delta, std::int64_t sequence)
    {
        healthy ();
        auto state = std::make_shared<MoveState> ();
        auto accepted = connector_.on<WorldState> ([state, sequence] (const auto &message) {
            if (message.payload.sequence != sequence)
                return;
            {
                std::lock_guard lock (state->mutex);
                if (state->result)
                    return;
                state->result = message.payload;
            }
            state->changed.notify_one ();
        });
        auto rejected = connector_.on<ActionRejected> ([state] (const auto &message) {
            {
                std::lock_guard lock (state->mutex);
                if (state->result)
                    return;
                state->result = message.payload;
            }
            state->changed.notify_one ();
        });
        (void) accepted;
        (void) rejected;
        connector_.send (MoveRequest{delta, sequence}).submit ();
        std::unique_lock lock (state->mutex);
        require (state->changed.wait_for (lock, Limits::packet, [&] { return state->result.has_value (); }),
                 "Move response was not received");
        auto result = *state->result;
        lock.unlock ();
        healthy ();
        return result;
    }
    WorldState move (double delta, std::int64_t sequence)
    {
        while (true) {
            auto result = move_result (delta, sequence);
            if (const auto *state = std::get_if<WorldState> (&result))
                return *state;
            const auto &rejection = std::get<ActionRejected> (result);
            if (rejection.reason != "move-budget" || rejection.retryAfterMs <= 0)
                throw std::runtime_error ("Move was rejected: " + rejection.reason);
            std::this_thread::sleep_for (std::chrono::milliseconds (rejection.retryAfterMs));
        }
    }
    ActionRejected move_rejected (double delta, std::int64_t sequence)
    {
        auto result = move_result (delta, sequence);
        if (const auto *rejection = std::get_if<ActionRejected> (&result))
            return *rejection;
        throw std::runtime_error ("Move unexpectedly succeeded");
    }
    WorldState state () { return exchange<JoinWorldRequest, WorldState> ({player_id, 0}); }
    SessionInfo info () { return exchange<SessionInfoRequest, SessionInfo> ({}); }
    DungeonOpened dungeon (const std::string &id)
    {
        return exchange<OpenDungeonRequest, DungeonOpened> (
          {id}, [&id] (const auto &opened) { return opened.dungeonId == id; });
    }
    template <class Message>
    ActionRejected packet_rejected (const Message &packet)
    {
        return exchange<Message, ActionRejected> (
          packet, [] (const auto &message) { return message.reason == "packet-not-allowed"; });
    }
    void close ()
    {
        healthy ();
        checked (connector_.close ());
    }
};
}
