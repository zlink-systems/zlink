#pragma once
#include "world.hpp"
#include <zlink/stream_connector.hpp>
#include <type_traits>
#include <variant>
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
class Client
{
    using Event = std::variant<RoomView, SessionInfo, ActionRejected, MatchEnded, MoveResult, ShotResult>;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Event> events_;
    std::vector<bool> consumed_;
    std::string error_;
    sc::connector_t connector_;
    std::vector<sc::subscription_t> subscriptions_;
    template <class T> void observe ()
    {
        subscriptions_.push_back (connector_.on<T> ([this] (const auto &message) {
            {
                std::lock_guard lock (mutex_);
                events_.emplace_back (message.payload);
                consumed_.push_back (false);
            }
            changed_.notify_all ();
        }));
    }

  public:
    const std::string player_id;
    Client (const std::string &endpoint, std::string id) : player_id (std::move (id))
    {
        sc::connector_options_t options;
        options.endpoint = endpoint;
        options.reconnect.enabled = false;
        options.wait_timeout = Limits::packet;
        options.dispatch_mode = sc::dispatch_mode_t::immediate;
        connector_ = sc::connector_factory_t::create (options);
        observe<RoomView> ();
        observe<MoveResult> ();
        observe<ShotResult> ();
        observe<SessionInfo> ();
        observe<ActionRejected> ();
        observe<MatchEnded> ();
        subscriptions_.push_back (connector_.on_error ([this] (const auto &error) {
            {
                std::lock_guard lock (mutex_);
                if (error_.empty ())
                    error_ = error.message;
            }
            changed_.notify_all ();
        }));
        checked (connector_.connect ());
    }
    void healthy ()
    {
        require (connector_.is_connected (), "Client disconnected");
        std::lock_guard lock (mutex_);
        require (error_.empty (), "Connector error: " + error_);
    }
    std::size_t mark ()
    {
        std::lock_guard lock (mutex_);
        return events_.size ();
    }
    template <class T, class Predicate> T receive (std::size_t from, Predicate predicate)
    {
        std::unique_lock lock (mutex_);
        std::optional<T> found;
        auto find = [&] {
            for (auto i = from; i < events_.size (); ++i) {
                if (consumed_[i])
                    continue;
                if (const auto *value = std::get_if<T> (&events_[i]); value && predicate (*value)) {
                    found = *value;
                    if constexpr (std::is_same_v<T, ActionRejected>)
                        consumed_[i] = true;
                    return true;
                }
                if constexpr (!std::is_same_v<T, ActionRejected>)
                    if (const auto *error = std::get_if<ActionRejected> (&events_[i]))
                        throw std::runtime_error ("Action rejected: " + error->reason);
            }
            return !error_.empty ();
        };
        require (changed_.wait_for (lock, Limits::packet, find),
                 std::string ("Packet deadline: ") + T::packet_name);
        require (found.has_value (), "Connector error: " + error_);
        lock.unlock ();
        healthy ();
        return *found;
    }
    template <class T> T receive (std::size_t from)
    {
        return receive<T> (from, [] (const T &) { return true; });
    }
    template <class Predicate> std::variant<RoomView, ActionRejected>
    receive_action (std::size_t from, Predicate predicate)
    {
        std::unique_lock lock (mutex_);
        std::optional<std::variant<RoomView, ActionRejected>> found;
        auto find = [&] {
            for (auto i = from; i < events_.size (); ++i) {
                if (consumed_[i])
                    continue;
                if (const auto *rejected = std::get_if<ActionRejected> (&events_[i])) {
                    found = *rejected;
                    consumed_[i] = true;
                    return true;
                }
                if (const auto *view = std::get_if<RoomView> (&events_[i]);
                    view && predicate (*view)) {
                    found = *view;
                    return true;
                }
            }
            return !error_.empty ();
        };
        require (changed_.wait_for (lock, Limits::packet, find), "Packet deadline: action outcome");
        require (found.has_value (), "Connector error: " + error_);
        lock.unlock ();
        healthy ();
        return *found;
    }
    template <class T> void send (const T &message)
    {
        healthy ();
        connector_.send (message).submit ();
    }
    RoomView join (const std::string &room)
    {
        const auto from = mark ();
        send (JoinRoomRequest{player_id, room});
        return receive<RoomView> (from);
    }
    RoomView view ()
    {
        const auto from = mark ();
        send (RoomViewRequest{});
        return receive<RoomView> (from);
    }
    SessionInfo info ()
    {
        const auto from = mark ();
        send (SessionInfoRequest{});
        const auto identity = receive<SessionInfo> (from);
        require (identity.actorId == player_id && identity.objectGeneration != 0
                   && !identity.sessionId.empty (),
                 "Invalid Session binding identity");
        return identity;
    }
    void close ()
    {
        healthy ();
        checked (connector_.close ());
    }
};
}
