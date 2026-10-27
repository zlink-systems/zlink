#pragma once
#include "world.hpp"
#include <zlink/stream_connector.hpp>
#include <cstddef>
#include <condition_variable>
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
inline bool same_dungeon_state (const DungeonView &a, const DungeonView &b)
{
    return a.dungeonId == b.dungeonId && a.partyId == b.partyId && a.runId == b.runId
           && a.phase == b.phase && a.members == b.members && a.stage == b.stage;
}
class Client
{
    using Received = std::variant<TownView, DungeonView, ActionRejected, SessionInfo>;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Received> received_;
    std::string failure_;
    int disconnects_ = 0;
    bool closed_ = false;
    sc::connector_t connector_;
    std::vector<sc::subscription_t> subscriptions_;

    template <class T> void observe ()
    {
        subscriptions_.push_back (connector_.on<T> ([this] (const auto &message) {
            {
                std::lock_guard lock (mutex_);
                received_.push_back (message.payload);
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
        options.dispatch_mode = sc::dispatch_mode_t::immediate;
        options.reconnect.enabled = false;
        options.wait_timeout = Limits::packet;
        connector_ = sc::connector_factory_t::create (options);
        observe<TownView> ();
        observe<DungeonView> ();
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
    ~Client ()
    {
        if (closed_)
            return;
        const auto result = connector_.close ();
        if (!result)
            std::cerr << "Client cleanup failed: "
                      << (result.error () ? result.error ()->message : "Connector close failed")
                      << std::endl;
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
            return std::find_if (received_.begin () + static_cast<std::ptrdiff_t> (from),
                                 received_.end (), [&] (const auto &item) {
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
    template <class T, class Predicate> T receive (Predicate predicate)
    {
        return wait<T> (0, std::move (predicate));
    }
    template <class T> T receive () { return wait<T> (0, [] (const T &) { return true; }); }
    template <class T> void send (const T &message)
    {
        healthy ();
        connector_.send (message).submit ();
    }
    template <class Reply, class Request, class Predicate>
    Reply exchange (const Request &request, Predicate predicate)
    {
        const auto from = mark ();
        send (request);
        const auto result = wait<Reply> (from, std::move (predicate));
        healthy ();
        return result;
    }
    template <class Reply, class Request> Reply exchange (const Request &request)
    {
        return exchange<Reply> (request, [] (const auto &) { return true; });
    }
    TownView join (std::int64_t channel)
    {
        return exchange<TownView> (
          JoinTownRequest{player_id, channel}, [this, channel] (const auto &view) {
              return view.townId == Names::town_id (channel)
                     && std::find (view.players.begin (), view.players.end (), player_id)
                          != view.players.end ();
          });
    }
    DungeonView reconnect () { return exchange<DungeonView> (JoinTownRequest{player_id, 2}); }
    SessionInfo info () { return exchange<SessionInfo> (SessionInfoRequest{}); }
    DungeonView dungeon (const DungeonView &expected, bool moved = false)
    {
        return exchange<DungeonView> (ViewRequest{}, [&] (const auto &view) {
            return same_dungeon_state (view, expected)
                   && (moved ? view.nodeId != expected.nodeId : view.nodeId == expected.nodeId);
        });
    }
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
