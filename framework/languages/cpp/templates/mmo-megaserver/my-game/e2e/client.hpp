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
    template <class Request, class Reply> Reply exchange (const Request &request)
    {
        healthy ();
        connector_.send (request).submit ();
        const auto reply = checked (connector_.wait_for<Reply> ().submit ()).payload;
        healthy ();
        return reply;
    }
    RegionView join (const std::string &region)
    {
        return exchange<EnterRegionRequest, RegionView> ({player_id, region});
    }
    RegionView view () { return exchange<ViewRequest, RegionView> ({}); }
    void move (int x, std::int64_t sequence)
    {
        healthy ();
        connector_.send (MoveRequest{x, sequence}).submit ();
    }
    template <class Predicate> RegionView wait_view (Predicate predicate)
    {
        return checked (connector_.wait_for<RegionView> ()
                          .where ([predicate] (const auto &message) {
                              return predicate (message.payload);
                          })
                          .submit ())
          .payload;
    }
    void no_region_push ()
    {
        healthy ();
        checked (connector_.expect_none<RegionView> ().within (Limits::isolation).submit ());
        healthy ();
    }
    SessionInfo info () { return exchange<SessionInfoRequest, SessionInfo> ({}); }
    DungeonOpened dungeon (const std::string &id)
    {
        return exchange<OpenDungeonRequest, DungeonOpened> ({id});
    }
    void close ()
    {
        healthy ();
        checked (connector_.close ());
    }
};
}
