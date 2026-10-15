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
    PlayerView enter () { return exchange<EnterWorldRequest, PlayerView> ({player_id}); }
    PlayerView earn (std::int64_t amount) { return exchange<EarnRequest, PlayerView> ({amount}); }
    PlayerView change (int channel)
    {
        return exchange<ChangeChannelRequest, PlayerView> ({channel});
    }
    PlayerView view () { return exchange<ViewRequest, PlayerView> ({}); }
    SessionInfo info () { return exchange<SessionInfoRequest, SessionInfo> ({}); }
    void close ()
    {
        healthy ();
        checked (connector_.close ());
    }
};
}
