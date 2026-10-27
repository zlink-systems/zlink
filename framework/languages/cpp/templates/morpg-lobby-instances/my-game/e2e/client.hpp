#pragma once
#include "world.hpp"
#include <algorithm>
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
    template <class Packet> void send (const Packet &packet)
    {
        healthy ();
        connector_.send (packet).submit ();
    }
    template <class Packet, class Predicate> Packet receive (Predicate predicate)
    {
        auto result = checked (connector_.wait_for<Packet> ()
                                 .where ([predicate] (const auto &packet) {
                                     return predicate (packet.payload);
                                 })
                                 .submit ())
                        .payload;
        healthy ();
        return result;
    }
    void join () { (void) exchange<JoinLobbyRequest, LobbyView> ({player_id}); }
    SessionInfo info () { return exchange<SessionInfoRequest, SessionInfo> ({}); }
    template <class Predicate> DungeonView view (Predicate predicate)
    {
        send (ViewRequest{});
        return receive<DungeonView> (predicate);
    }
    PartyView party_view (const std::string &id, std::size_t count, std::size_t ready)
    {
        return receive<PartyView> ([=] (const auto &view) {
            return view.partyId == id && view.members.size () == count
                   && view.ready.size () == count
                   && static_cast<std::size_t> (
                        std::count (view.ready.begin (), view.ready.end (), true))
                        == ready;
        });
    }
    DungeonView dungeon_view (const std::string &id, std::int64_t stage)
    {
        return receive<DungeonView> (
          [=] (const auto &view) { return view.partyId == id && view.stage == stage && view.phase == (stage == Names::final_stage ? RunPhase::Completed : RunPhase::Active); });
    }
    void close ()
    {
        healthy ();
        checked (connector_.close ());
    }
};
}
