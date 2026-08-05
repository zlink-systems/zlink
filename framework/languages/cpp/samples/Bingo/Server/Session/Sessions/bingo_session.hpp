/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Configuration/sample_names.hpp"
#include "../../Configuration/sample_topology.hpp"
#include "Handlers/authenticate_session_handler.hpp"

#include <optional>
#include <string>

namespace zlink::samples::bingo
{

using namespace framework;
using framework::actor_ref_t;
using framework::message_t;

class bingo_session_t final : public packet_stream_session_t
{
  public:
    using dependency_types =
      dependency_list_t<session_actor_manager_t, authenticate_session_handler_t>;

    bingo_session_t (session_actor_manager_t &actors,
                     authenticate_session_handler_t &authenticate) :
        _actors (actors), _authenticate (authenticate)
    {
    }

    task_t<void> on_connected (stream_t &) override
    {
        return task_t<void> (result_t<void>::success ());
    }

    task_t<void> on_disconnected (stream_t &) override
    {
        if (_bound_actor_id) {
            if (auto actor = _actors.find (*_bound_actor_id)) {
                co_await actor->notify_disconnected ();
            }
            _bound_actor_id.reset ();
        }
        co_return;
    }

    task_t<void> on_error (stream_t &, const stream_error_t &) override
    {
        return task_t<void> (result_t<void>::success ());
    }

    task_t<void> on_packet (stream_t &stream,
                            const session_message_context_t &dispatch,
                            const zlink::message_t &payload) override
    {
        if (_authenticate.can_handle (dispatch)) {
            auto authenticated = co_await _authenticate.handle (_actors, stream, payload);
            _bound_actor_id = std::string (authenticated.actor_id ());
            co_return;
        }

        auto actor = require_bound_actor (std::string ("relaying packet '")
                                          + std::string (dispatch.packet_name) + "'");
        if (!actor) {
            co_return;
        }
        if (dispatch.can_reply) {
            auto reply = co_await actor.value ().relay_request (payload).submit ();
            stream.reply_packet (reply).submit ();
            co_return;
        }
        co_await actor.value ().relay (payload);
        co_return;
    }

  private:
    result_t<session_actor_t> require_bound_actor (const std::string &action) const
    {
        if (!_bound_actor_id) {
            return result_t<session_actor_t>::failure (framework_error_kind_t::internal_failure,
                                                       "Client must authenticate before " + action
                                                         + ".");
        }
        auto actor = _actors.find (*_bound_actor_id);
        if (!actor) {
            return result_t<session_actor_t>::failure (
              framework_error_kind_t::not_found,
              "Exactly one actor must be bound before " + action + ".");
        }
        return result_t<session_actor_t>::success (std::move (*actor));
    }

    session_actor_manager_t &_actors;
    authenticate_session_handler_t &_authenticate;
    std::optional<std::string> _bound_actor_id;
};

} // namespace zlink::samples::bingo
