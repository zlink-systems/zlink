#pragma once
#include "world.hpp"
#include <zlink/stream_connector.hpp>
#include <future>

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
    template <class Request, class Reply, class Predicate>
    Reply exchange (const Request &request, Predicate predicate)
    {
        healthy ();
        auto reply_promise =
          std::make_shared<std::promise<sc::result_t<sc::message_t<Reply>>>> ();
        auto pending_reply = reply_promise->get_future ();
        connector_.wait_for<Reply> ()
          .where ([predicate] (const auto &message) { return predicate (message.payload); })
          .submit ([reply_promise] (auto result) {
              reply_promise->set_value (std::move (result));
          });
        connector_.send (request).submit ();
        const auto reply = checked (pending_reply.get ()).payload;
        healthy ();
        return reply;
    }
    WorldState join (double x)
    {
        return exchange<JoinWorldRequest, WorldState> ({player_id, x}, [x] (const auto &view) {
            return view.fieldId == Names::field_id (x) && view.x == x && view.sequence == 0;
        });
    }
    WorldState move (double delta, std::int64_t sequence, double expected_x)
    {
        return exchange<MoveRequest, WorldState> (
          {delta, sequence}, [expected_x, sequence] (const auto &view) {
            return view.x == expected_x && view.sequence == sequence;
        });
    }
    SessionInfo info ()
    {
        return exchange<SessionInfoRequest, SessionInfo> ({}, [] (const auto &) { return true; });
    }
    DungeonView dungeon (const DungeonView &expected, const std::string &previous_host = "")
    {
        return exchange<ViewRequest, DungeonView> (
          {}, [expected, previous_host] (const auto &view) {
              return view.nodeId != previous_host && view.dungeonId == expected.dungeonId
                  && view.partyId == expected.partyId && view.members == expected.members
                  && view.stage == expected.stage && view.runId == expected.runId
                  && view.phase == expected.phase;
          });
    }
    PartyView party ()
    {
        return exchange<ViewRequest, PartyView> ({}, [] (const auto &) { return true; });
    }
    DungeonView reconnect_dungeon ()
    {
        return exchange<JoinWorldRequest, DungeonView> ({player_id, 250},
                                                        [] (const auto &) { return true; });
    }
    template <class T, class Predicate> T receive (Predicate predicate)
    {
        healthy ();
        auto result = connector_.wait_for<T> ()
                        .where ([predicate] (const auto &message) {
                            return predicate (message.payload);
                        })
                        .submit ();
        require (static_cast<bool> (result),
                 std::string ("Waiting for ") + T::packet_name + " failed: "
                   + (result.error () ? result.error ()->message : "Connector wait failed"));
        auto payload = result.value ().payload;
        healthy ();
        return payload;
    }
    template <class T, class Predicate> std::future<T> receive_async (Predicate predicate)
    {
        healthy ();
        auto reply_promise =
          std::make_shared<std::promise<sc::result_t<sc::message_t<T>>>> ();
        auto pending_reply = reply_promise->get_future ();
        connector_.wait_for<T> ()
          .where ([predicate] (const auto &message) { return predicate (message.payload); })
          .submit ([reply_promise] (auto result) {
              reply_promise->set_value (std::move (result));
          });
        return std::async (std::launch::deferred,
                           [this, pending_reply = std::move (pending_reply)] () mutable {
                               const auto payload = checked (pending_reply.get ()).payload;
                               healthy ();
                               return payload;
                           });
    }
    template <class T> void send (const T &request)
    {
        healthy ();
        connector_.send (request).submit ();
    }
    auto border (const std::string &source)
    {
        return connector_.wait_for<BorderVisible> ().where ([source] (const auto &message) {
            return message.payload.playerId == source && message.payload.sequence == 1;
        });
    }
    void close ()
    {
        healthy ();
        checked (connector_.close ());
    }
};
}
