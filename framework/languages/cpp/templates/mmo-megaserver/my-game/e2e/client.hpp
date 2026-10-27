#pragma once
#include "world.hpp"
#include <zlink/stream_connector.hpp>
#include <algorithm>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>

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
    std::mutex view_mutex_;
    std::condition_variable view_changed_;
    std::vector<RegionView> views_;
    sc::connector_t connector_;
    sc::subscription_t error_subscription_;
    sc::subscription_t view_subscription_;

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
        view_subscription_ = connector_.on<RegionView> ([this] (const auto &message) {
            {
                std::lock_guard lock (view_mutex_);
                views_.push_back (message.payload);
            }
            view_changed_.notify_all ();
        });
        checked (connector_.connect ());
    }
    void healthy ()
    {
        require (connector_.is_connected (), "Client disconnected");
        std::lock_guard lock (error_mutex_);
        require (error_.empty (), "Connector error: " + error_);
    }
    template <class Request, class Reply, class Predicate>
    Reply exchange (const Request &request, Predicate predicate)
    {
        healthy ();
        connector_.send (request).submit ();
        const auto reply = checked (connector_.wait_for<Reply> ()
                                      .where ([predicate] (const auto &message) {
                                          return predicate (message.payload);
                                      })
                                      .submit ())
                             .payload;
        healthy ();
        return reply;
    }
    template <class Request, class Reply> Reply exchange (const Request &request)
    {
        return exchange<Request, Reply> (request, [] (const auto &) { return true; });
    }
    RegionView join (const std::string &region)
    {
        const auto from = mark_views ();
        healthy ();
        connector_.send (EnterRegionRequest{player_id, region}).submit ();
        return wait_view (from, [] (const auto &) { return true; });
    }
    std::size_t mark_views ()
    {
        std::lock_guard lock (view_mutex_);
        return views_.size ();
    }
    std::size_t view_count () { return mark_views (); }
    template <class Predicate> RegionView wait_view (std::size_t from, Predicate predicate)
    {
        std::unique_lock lock (view_mutex_);
        const auto find = [&] {
            const auto begin = views_.begin () + std::min (from, views_.size ());
            return std::find_if (begin, views_.end (), predicate);
        };
        require (view_changed_.wait_for (lock, Limits::packet,
                                         [&] { return find () != views_.end (); }),
                 "Stream connector wait timed out: RegionView");
        return *find ();
    }
    template <class Predicate> RegionView wait_view (Predicate predicate)
    {
        return wait_view (0, std::move (predicate));
    }
    template <class Predicate> RegionView view (Predicate predicate)
    {
        const auto from = mark_views ();
        healthy ();
        connector_.send (ViewRequest{}).submit ();
        return wait_view (from, std::move (predicate));
    }
    void move (double delta, std::int64_t sequence)
    {
        healthy ();
        connector_.send (MoveRequest{delta, sequence}).submit ();
    }
    ActionRejected wait_rejection ()
    {
        const auto rejected = checked (connector_.wait_for<ActionRejected> ().submit ()).payload;
        healthy ();
        return rejected;
    }
    void no_region_push ()
    {
        const auto before = view_count ();
        healthy ();
        checked (connector_.expect_none<RegionView> ().within (Limits::isolation).submit ());
        healthy ();
        require (view_count () == before, "Unexpected Region update");
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
