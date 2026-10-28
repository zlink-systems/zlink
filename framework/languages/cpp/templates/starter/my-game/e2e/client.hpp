#pragma once
#include "world.hpp"
#include <zlink/stream_connector.hpp>

namespace MyGame::E2E
{
namespace sc = zlink::stream_connector;
inline void checked (sc::result_t<void> result)
{
    require (static_cast<bool> (result),
             result.error () ? result.error ()->message : "Connector operation failed");
}
// A public STREAM connector. Add typed listeners and sends when packets are introduced.
class Client
{
    std::mutex mutex_;
    std::string error_;
    bool closing_ = false;
    sc::connector_t connector_;
    std::vector<sc::subscription_t> subscriptions_;

  public:
    explicit Client (const std::string &endpoint)
    {
        sc::connector_options_t options;
        options.endpoint = endpoint;
        options.reconnect.enabled = false;
        options.dispatch_mode = sc::dispatch_mode_t::immediate;
        options.wait_timeout = Limits::packet;
        connector_ = sc::connector_factory_t::create (options);
        subscriptions_.push_back (connector_.on_error ([this] (const auto &error) {
            std::lock_guard lock (mutex_);
            if (error_.empty ())
                error_ = error.message;
        }));
        subscriptions_.push_back (connector_.on_disconnected ([this] (auto) {
            std::lock_guard lock (mutex_);
            if (!closing_ && error_.empty ())
                error_ = "Unexpected client disconnect";
        }));
        checked (connector_.connect ());
    }
    void healthy ()
    {
        require (connector_.is_connected (), "Client disconnected");
        std::lock_guard lock (mutex_);
        require (error_.empty (), error_);
    }
    void close ()
    {
        healthy ();
        {
            std::lock_guard lock (mutex_);
            closing_ = true;
        }
        checked (connector_.close ());
    }
};
}
