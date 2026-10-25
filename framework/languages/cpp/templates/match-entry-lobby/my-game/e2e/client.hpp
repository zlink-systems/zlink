#pragma once
#include "world.hpp"
#include <zlink/stream_connector.hpp>
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
    using Received = std::variant<RoomView, MatchFound, MatchEnded, ActionRejected, SessionInfo>;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Received> received_;
    std::string failure_;
    int disconnects_ = 0;
    bool closed_ = false;
    std::ofstream log_;
    sc::connector_t connector_;
    std::vector<sc::subscription_t> subscriptions_;
    template <class T> void observe ()
    {
        subscriptions_.push_back (connector_.on<T> ([this] (const auto &message) {
            {
                std::lock_guard lock (mutex_);
                received_.push_back (message.payload);
                log_ << nlohmann::json{{"packet", T::packet_name}, {"payload", message.payload}}
                          .dump ()
                     << std::endl;
                if (!log_)
                    failure_ = "Client log write failed";
            }
            changed_.notify_all ();
        }));
    }

  public:
    const std::string player_id;
    Client (const std::string &endpoint, std::string id, const std::filesystem::path &log) :
        log_ (log), player_id (std::move (id))
    {
        require (static_cast<bool> (log_), "Cannot create client log");
        sc::connector_options_t options;
        options.endpoint = endpoint;
        options.dispatch_mode = sc::dispatch_mode_t::immediate;
        options.reconnect.enabled = false;
        options.wait_timeout = Limits::packet;
        connector_ = sc::connector_factory_t::create (options);
        observe<RoomView> ();
        observe<MatchFound> ();
        observe<MatchEnded> ();
        observe<ActionRejected> ();
        observe<SessionInfo> ();
        subscriptions_.push_back (connector_.on_error ([this] (const auto &error) {
            {
                std::lock_guard lock (mutex_);
                if (failure_.empty ())
                    failure_ = error.message;
            }
            changed_.notify_all ();
        }));
        subscriptions_.push_back (connector_.on_disconnected ([this] (auto) {
            {
                std::lock_guard lock (mutex_);
                ++disconnects_;
            }
            changed_.notify_all ();
        }));
        checked (connector_.connect ());
    }
    std::size_t mark ()
    {
        std::lock_guard lock (mutex_);
        return received_.size ();
    }
    void healthy ()
    {
        require (connector_.is_connected (), "Client disconnected");
        std::lock_guard lock (mutex_);
        require (failure_.empty (), "Connector error: " + failure_);
        require (disconnects_ == 0, "Unexpected disconnect");
    }
    template <class T, class Predicate> T wait (std::size_t from, Predicate predicate)
    {
        std::unique_lock lock (mutex_);
        const auto find = [&] () {
            return std::find_if (received_.begin () + from, received_.end (),
                                 [&] (const auto &item) {
                                     const auto *value = std::get_if<T> (&item);
                                     return value && predicate (*value);
                                 });
        };
        require (changed_.wait_for (lock, Limits::packet,
                                    [&] {
                                        return find () != received_.end () || !failure_.empty ()
                                               || disconnects_ != 0;
                                    }),
                 std::string ("Packet deadline: ") + T::packet_name);
        require (failure_.empty (), "Connector error: " + failure_);
        require (disconnects_ == 0, "Disconnected waiting for packet");
        const auto result = find ();
        require (result != received_.end (), "Missing packet");
        return std::get<T> (*result);
    }
    template <class T> T wait (std::size_t from = 0)
    {
        return wait<T> (from, [] (const T &) { return true; });
    }

    template <class Predicate>
    std::variant<RoomView, ActionRejected> wait_action (std::size_t from, Predicate updated)
    {
        std::unique_lock lock (mutex_);
        const auto find = [&] () {
            return std::find_if (received_.begin () + from, received_.end (), [&] (const auto &item) {
                if (std::holds_alternative<ActionRejected> (item))
                    return true;
                const auto *view = std::get_if<RoomView> (&item);
                return view && updated (*view);
            });
        };
        require (changed_.wait_for (lock, Limits::packet, [&] {
                     return find () != received_.end () || !failure_.empty () || disconnects_ != 0;
                 }),
                 "Packet deadline: RoomView or ActionRejected");
        require (failure_.empty (), "Connector error: " + failure_);
        require (disconnects_ == 0, "Disconnected waiting for action result");
        const auto result = find ();
        require (result != received_.end (), "Missing action result");
        if (const auto *rejection = std::get_if<ActionRejected> (&*result))
            return *rejection;
        return *std::get_if<RoomView> (&*result);
    }

    template <class T> void send (const T &message)
    {
        healthy ();
        connector_.send (message).submit ();
    }
    void queue () { send (QueueRequest{player_id}); }
    template <class Request, class Reply> Reply exchange (const Request &request)
    {
        const auto from = mark ();
        send (request);
        auto result = wait<Reply> (from);
        healthy ();
        return result;
    }
    SessionInfo info () { return exchange<SessionInfoRequest, SessionInfo> ({}); }
    RoomView view () { return exchange<RoomViewRequest, RoomView> ({}); }
    void close ()
    {
        if (closed_)
            return;
        healthy ();
        checked (connector_.close ());
        closed_ = true;
    }
};
}
