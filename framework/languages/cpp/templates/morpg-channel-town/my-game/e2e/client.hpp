#pragma once
#include "world.hpp"
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
inline bool same_dungeon_state (const DungeonView &a, const DungeonView &b)
{
    return a.dungeonId == b.dungeonId && a.partyId == b.partyId && a.members == b.members
           && a.stage == b.stage;
}
class Client
{
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
    template <class Reply, class Predicate> Reply receive (Predicate predicate)
    {
        const auto reply = checked (connector_.wait_for<Reply> ()
                                      .where ([predicate] (const auto &message) {
                                          return predicate (message.payload);
                                      })
                                      .submit ())
                             .payload;
        healthy ();
        return reply;
    }
    template <class Reply, class Request, class Predicate>
    Reply exchange (const Request &request, Predicate predicate)
    {
        healthy ();
        connector_.send (request).submit ();
        return receive<Reply> (predicate);
    }
    template <class Reply, class Request> Reply exchange (const Request &request)
    {
        return exchange<Reply> (request, [] (const auto &) { return true; });
    }
    TownView join (int channel)
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
        healthy ();
        checked (connector_.close ());
    }
};
}
