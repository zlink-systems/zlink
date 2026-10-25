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
    using Event = std::variant<TableView, HandDealt, HandResult, ActionRejected, SessionInfo>;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Event> pending_, history_;
    std::string error_;
    bool closing_ = false;
    sc::connector_t connector_;
    std::vector<sc::subscription_t> subscriptions_;
    template <class T> void listen ()
    {
        subscriptions_.push_back (connector_.on<T> ([this] (const auto &message) {
            {
                std::lock_guard lock (mutex_);
                pending_.emplace_back (message.payload);
                history_.emplace_back (message.payload);
            }
            changed_.notify_all ();
        }));
    }

  public:
    const std::string player_id, table_id;
    Client (const std::string &endpoint, std::string player, std::string table) :
        player_id (std::move (player)), table_id (std::move (table))
    {
        sc::connector_options_t options;
        options.endpoint = endpoint;
        options.reconnect.enabled = false;
        options.dispatch_mode = sc::dispatch_mode_t::immediate;
        options.wait_timeout = Limits::packet;
        connector_ = sc::connector_factory_t::create (options);
        listen<TableView> ();
        listen<HandDealt> ();
        listen<HandResult> ();
        listen<ActionRejected> ();
        listen<SessionInfo> ();
        subscriptions_.push_back (connector_.on_error ([this] (const auto &error) {
            {
                std::lock_guard lock (mutex_);
                if (error_.empty ())
                    error_ = error.message;
            }
            changed_.notify_all ();
        }));
        subscriptions_.push_back (connector_.on_disconnected ([this] (auto) {
            {
                std::lock_guard lock (mutex_);
                if (!closing_ && error_.empty ())
                    error_ = "Unexpected client disconnect";
            }
            changed_.notify_all ();
        }));
        checked (connector_.connect ());
    }
    void healthy ()
    {
        require (connector_.is_connected (), "Client disconnected");
        std::lock_guard lock (mutex_);
        require (error_.empty (), error_);
    }
    template <class T> void send (const T &message)
    {
        healthy ();
        connector_.send (message).submit ();
    }
    template <class T, class Predicate> T next (Predicate predicate)
    {
        std::unique_lock lock (mutex_);
        const auto match = [&] {
            return std::find_if (pending_.begin (), pending_.end (), [&] (const auto &event) {
                const auto *value = std::get_if<T> (&event);
                return value && predicate (*value);
            });
        };
        require (
          changed_.wait_for (lock, Limits::packet,
                             [&] { return !error_.empty () || match () != pending_.end (); }),
          "Packet deadline: " + std::string (T::packet_name));
        require (error_.empty (), error_);
        const auto found = match ();
        const auto result = std::get<T> (*found);
        pending_.erase (found);
        return result;
    }
    template <class T> T next ()
    {
        return next<T> ([] (const auto &) { return true; });
    }
    TableView join ()
    {
        send (JoinTableRequest{player_id, table_id});
        return next<TableView> ();
    }
    SessionInfo info ()
    {
        send (SessionInfoRequest{});
        return next<SessionInfo> ();
    }
    void clear ()
    {
        healthy ();
        std::lock_guard lock (mutex_);
        pending_.clear ();
    }
    void privacy (const HandDealt &own)
    {
        std::lock_guard lock (mutex_);
        for (const auto &event : history_) {
            require (!std::holds_alternative<HandResult> (event),
                     "Result arrived before settlement");
            if (const auto *hand = std::get_if<HandDealt> (&event); hand && !hand->cards.empty ())
                require (*hand == own, "Another player's private hand was delivered");
            if (const auto *view = std::get_if<TableView> (&event)) {
                const nlohmann::json public_view = *view;
                for (const auto &seat : public_view.at ("seats"))
                    require (!seat.contains ("cards"), "Public view contains private cards");
            }
        }
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
