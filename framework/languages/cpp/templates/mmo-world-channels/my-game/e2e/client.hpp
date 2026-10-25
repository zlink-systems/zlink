#pragma once
#include "world.hpp"
#include <zlink/stream_connector.hpp>
#include <functional>
#include <condition_variable>
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
    sc::connector_t connector_;
    sc::subscription_t error_subscription_;
    sc::subscription_t channel_view_subscription_;
    std::mutex channel_view_mutex_;
    std::condition_variable channel_view_changed_;
    std::vector<ChannelView> channel_views_;
    bool closed_ = false;

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
        channel_view_subscription_ = connector_.on<ChannelView> ([this] (const auto &message) {
            {
                std::lock_guard lock (channel_view_mutex_);
                channel_views_.push_back (message.payload);
            }
            channel_view_changed_.notify_all ();
        });
        error_subscription_ = connector_.on_error ([this] (const auto &error) {
            std::lock_guard lock (error_mutex_);
            if (error_.empty ())
                error_ = error.message;
        });
        checked (connector_.connect ());
    }
    void healthy ()
    {
        if (closed_)
            return;
        require (connector_.is_connected (), "Client " + player_id + " disconnected");
        std::lock_guard lock (error_mutex_);
        require (error_.empty (), "Connector error: " + error_);
    }
    template <class Request, class Reply>
    Reply exchange (const Request &request, std::function<bool (const Reply &)> predicate = {})
    {
        healthy ();
        connector_.send (request).submit ();
        const auto reply =
          checked (connector_.wait_for<Reply> ()
                     .where ([predicate = std::move (predicate)] (const auto &message) {
                         return !predicate || predicate (message.payload);
                     })
                     .submit ())
            .payload;
        healthy ();
        return reply;
    }
    PlayerView enter () { return exchange<EnterWorldRequest, PlayerView> ({player_id}); }
    PlayerView earn (std::int64_t expected_gold)
    {
        return exchange<EarnRequest, PlayerView> (
          {}, [expected_gold] (const auto &view) { return view.gold == expected_gold; });
    }
    ActionRejected earn_rejected ()
    {
        return exchange<EarnRequest, ActionRejected> ({}, [] (const auto &) { return true; });
    }
    ActionRejected reject_internal (const PlayerView &request)
    {
        return exchange<PlayerView, ActionRejected> (request, [] (const auto &) { return true; });
    }
    ActionRejected reject_channel (std::int64_t channel)
    {
        return exchange<ChangeChannelRequest, ActionRejected> (
          {channel}, [] (const auto &) { return true; });
    }
    std::size_t mark_channel_views ()
    {
        std::lock_guard lock (channel_view_mutex_);
        return channel_views_.size ();
    }
    ChannelView wait_channel_view (std::size_t from,
                                   std::function<bool (const ChannelView &)> predicate)
    {
        std::unique_lock lock (channel_view_mutex_);
        const auto find = [&] {
            return std::find_if (channel_views_.begin () + static_cast<std::ptrdiff_t> (from),
                                 channel_views_.end (), predicate);
        };
        require (channel_view_changed_.wait_for (lock, Limits::packet,
                                                 [&] { return find () != channel_views_.end (); }),
                 "Packet deadline: ChannelView");
        const auto reply = *find ();
        lock.unlock ();
        healthy ();
        return reply;
    }
    ChannelView channel_view (std::function<bool (const ChannelView &)> predicate)
    {
        const auto from = mark_channel_views ();
        healthy ();
        connector_.send (ChannelViewRequest{}).submit ();
        return wait_channel_view (from, std::move (predicate));
    }
    ChannelView channel_view () { return channel_view ([] (const auto &) { return true; }); }
    PlayerView change (std::int64_t channel)
    {
        return exchange<ChangeChannelRequest, PlayerView> (
          {channel}, [channel] (const auto &view) { return view.channel == channel; });
    }
    PlayerView view (const PlayerView &expected)
    {
        return exchange<ViewRequest, PlayerView> (
          {}, [expected] (const auto &view) { return view == expected; });
    }
    SessionInfo info () { return exchange<SessionInfoRequest, SessionInfo> ({}); }
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
