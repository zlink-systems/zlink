/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Configuration/sample_names.hpp"
#include "../Configuration/sample_configuration.hpp"
#include "../Configuration/sample_readiness.hpp"
#include "Application/ConversationAssignment/agent_assignment_service.hpp"
#include "Domain/SupportChat/conversation.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace zlink::samples::supportchat
{
using namespace zlink::framework;

inline constexpr const char *support_user_actor_type = "support-user";

class support_user_actor_t : public actor_t
{
  public:
    explicit support_user_actor_t (actor_context_t value) :
        actor_id (value.actor_ref ().actor_id ().value ()),
        actor_ref (value.actor_ref ()),
        actor_context (std::move (value))
    {
    }

    actor_context_t &context () noexcept override { return actor_context; }
    const actor_context_t &context () const noexcept override { return actor_context; }

    join_conversation_res_t schedule_conversation_join (const std::string &conversation_id,
                                                        bool notify_bound_session)
    {
        _pending_joins.push_back ({conversation_id, notify_bound_session});
        try {
            actor_context
              .join_spot (
                spot_id_t (conversation_id),
                join_conversation_req_t{conversation_id, participant_id, role, display_name})
              .defer ();
        }
        catch (...) {
            _pending_joins.pop_back ();
            throw;
        }
        return {true,
                actor_id,
                conversation_state_t{conversation_id,
                                     {},
                                     conversation_status_t::waiting_for_agent,
                                     role == role_t::customer ? participant_id : std::string{},
                                     std::nullopt,
                                     0,
                                     std::nullopt,
                                     std::nullopt}};
    }

    task_t<void> on_join_completed (const actor_join_completion_t &completion) override
    {
        const auto operation_key = std::visit (
          [] (const auto &value) {
              return std::to_string (value.operation_id_high) + ":"
                     + std::to_string (value.operation_id_low);
          },
          completion);
        if (_completed_join_operations.contains (operation_key)) {
            co_return;
        }

        std::string conversation_id;
        bool notify_bound_session = role == role_t::agent;
        if (!_pending_joins.empty ()) {
            conversation_id = _pending_joins.front ().conversation_id;
            notify_bound_session = _pending_joins.front ().notify_bound_session;
        } else {
            const std::optional<zlink::framework::message_t> *reply = nullptr;
            if (const auto *accepted = std::get_if<actor_join_accepted_t> (&completion)) {
                reply = &accepted->reply;
            } else if (const auto *rejected = std::get_if<actor_join_rejected_t> (&completion)) {
                reply = &rejected->reply;
            }
            if (reply != nullptr && reply->has_value ()) {
                conversation_id = reply->value ()
                                    .decode<join_conversation_res_t> ()
                                    .state.conversation_id;
            }
        }

        if (const auto *accepted = std::get_if<actor_join_accepted_t> (&completion)) {
            if (accepted->reply) {
                current_conversation_id = accepted->reply->decode<join_conversation_res_t> ()
                                            .state.conversation_id;
            } else if (!conversation_id.empty ()) {
                current_conversation_id = conversation_id;
            }
        } else if (notify_bound_session && !conversation_id.empty ()) {
            if (std::holds_alternative<actor_join_rejected_t> (completion)) {
                co_await actor_context.bound_session ()
                  .send (join_conversation_failed_notify_t{conversation_id, "Rejected"})
                  .async ();
            } else {
                const auto &failed = std::get<actor_join_failed_t> (completion);
                co_await actor_context.bound_session ()
                  .send (join_conversation_failed_notify_t{
                    conversation_id, std::to_string (static_cast<int> (failed.error_kind))})
                  .async ();
            }
        }

        if (!_pending_joins.empty ()) {
            _pending_joins.pop_front ();
        }
        _completed_join_operations.insert (operation_key);
        co_return;
    }

    const std::set<std::string> &completed_join_operations () const noexcept
    {
        return _completed_join_operations;
    }

    void restore_completed_join_operations (std::set<std::string> values)
    {
        _completed_join_operations = std::move (values);
    }

    std::string actor_id;
    std::string display_name;
    std::string role;
    std::string participant_id;
    std::string current_conversation_id;
    zlink::framework::actor_ref_t actor_ref;
    actor_context_t actor_context;

  private:
    struct pending_join_t
    {
        std::string conversation_id;
        bool notify_bound_session{false};
    };

    std::deque<pending_join_t> _pending_joins;
    std::set<std::string> _completed_join_operations;
};

struct support_user_actor_relocation_state_t
{
    std::string display_name;
    std::string role;
    std::string participant_id;
    std::string conversation_id;
    std::set<std::string> completed_join_operations;
};

inline void to_json (nlohmann::json &json, const support_user_actor_relocation_state_t &value)
{
    json = {{"displayName", value.display_name},
            {"role", value.role},
            {"participantId", value.participant_id},
            {"conversationId", value.conversation_id},
            {"completedJoinOperations", value.completed_join_operations}};
}

inline void from_json (const nlohmann::json &json, support_user_actor_relocation_state_t &value)
{
    value.display_name = json.value ("displayName", std::string{});
    value.role = json.value ("role", std::string{});
    value.participant_id = json.value ("participantId", std::string{});
    value.conversation_id = json.value ("conversationId", std::string{});
    value.completed_join_operations = json.value ("completedJoinOperations",
                                                  std::set<std::string>{});
}

class support_user_actor_relocation_adapter_t final
    : public actor_relocation_adapter_t<support_user_actor_t>
{
  public:
    task_t<std::vector<std::byte>> capture (support_user_actor_t &actor, std::stop_token) override
    {
        const auto message = zlink::message_t::from_json (
          support_user_actor_relocation_state_t{actor.display_name,
                                                actor.role,
                                                actor.participant_id,
                                                actor.current_conversation_id,
                                                actor.completed_join_operations ()});
        co_return std::vector<std::byte> (message.bytes ().begin (), message.bytes ().end ());
    }

    task_t<void>
    restore (support_user_actor_t &actor, std::vector<std::byte> payload, std::stop_token) override
    {
        const auto message = zlink::message_t::from (
          std::span<const std::byte> (payload.data (), payload.size ()));
        auto relocated = message.parse_json<support_user_actor_relocation_state_t> ();
        actor.display_name = std::move (relocated.display_name);
        actor.role = std::move (relocated.role);
        actor.participant_id = std::move (relocated.participant_id);
        actor.current_conversation_id = std::move (relocated.conversation_id);
        actor.restore_completed_join_operations (std::move (relocated.completed_join_operations));
        co_return;
    }
};

class supportchat_conversation_runtime_t
{
  public:
    static std::string conversation_actor_id (const std::string &roster_actor_id,
                                              const std::string &conversation_id)
    {
        return roster_actor_id + "@" + conversation_id;
    }

    struct actor_profile_t
    {
        std::string actor_id;
        std::string display_name;
        std::string role;
        std::string participant_id;
    };

    void remember_actor (const std::string &actor_id,
                         const std::string &display_name,
                         const std::string &role,
                         const std::string &participant_id)
    {
        std::lock_guard lock (_mutex);
        _actors[actor_id] = participant_t{actor_id, display_name, role, participant_id};
    }

    set_agent_available_res_t set_agent_available (const std::string &actor_id,
                                                   const std::string &display_name,
                                                   bool available)
    {
        std::lock_guard lock (_mutex);
        _assignment.set_available (actor_id, display_name, available);
        return {available};
    }

    std::optional<std::string> assign_agent (const std::string &conversation_id)
    {
        std::lock_guard lock (_mutex);
        const auto assigned = _assignment.assign_for_conversation (conversation_id);
        return assigned ? std::optional<std::string>{assigned->roster_actor_id} : std::nullopt;
    }

    void release_conversation (const std::string &conversation_id)
    {
        std::lock_guard lock (_mutex);
        _assignment.release_conversation (conversation_id);
    }

    std::optional<actor_profile_t> actor_profile (const std::string &actor_id) const
    {
        std::lock_guard lock (_mutex);
        const auto found = _actors.find (actor_id);
        if (found == _actors.end ()) {
            return std::nullopt;
        }
        return actor_profile_t{found->second.actor_id,
                               found->second.display_name,
                               found->second.role,
                               found->second.participant_id};
    }

    std::optional<support_user_actor_t *> actor_for (const std::string &participant_id,
                                                     const std::string &conversation_id = {}) const
    {
        std::lock_guard lock (_mutex);
        auto actor_id = actor_id_for_participant (participant_id);
        const auto identity = _actors.find (participant_id);
        if (!conversation_id.empty () && identity != _actors.end ()
            && identity->second.role == role_t::agent) {
            actor_id = conversation_actor_id (actor_id, conversation_id);
        }
        const auto found = _live_actors.find (actor_id);
        if (found == _live_actors.end ()) {
            return std::nullopt;
        }
        return found->second;
    }

    void remember_live_actor (support_user_actor_t &actor)
    {
        std::lock_guard lock (_mutex);
        _live_actors[actor.actor_id] = &actor;
        if (actor.participant_id == actor.actor_id) {
            _live_actors[actor.participant_id] = &actor;
        }
    }

  private:
    struct participant_t
    {
        std::string actor_id;
        std::string display_name;
        std::string role;
        std::string participant_id;
    };

    std::string actor_id_for_participant (const std::string &participant_id) const
    {
        const auto identity = _actors.find (participant_id);
        if (identity != _actors.end () && identity->second.participant_id == participant_id) {
            return identity->second.actor_id;
        }
        return participant_id;
    }

    mutable std::mutex _mutex;
    std::map<std::string, participant_t> _actors;
    std::map<std::string, support_user_actor_t *> _live_actors;
    agent_availability_directory_t _agent_availability{3};
    agent_assignment_service_t _assignment{_agent_availability};
};

class conversation_spot_t;

/* conversation Spot의 유휴 tick handler(공통 sample spec §14). */
struct conversation_idle_timer_handler_t
{
    task_t<void> handle (conversation_spot_t &spot,
                         const zlink::framework::timer_tick_t &tick) const;
};

class conversation_spot_t : public spot_t<support_user_actor_t>
{
  public:
    conversation_spot_t (spot_context_t context, supportchat_conversation_runtime_t &runtime) :
        _runtime (runtime), _context (std::move (context))
    {
    }

    spot_context_t &context () noexcept override { return _context; }
    const spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ()
          .add_actor_request<&conversation_spot_t::join> (join_conversation_req_t::packet_name)
          .add_actor_request<&conversation_spot_t::send_message> (
            send_chat_message_req_t::packet_name)
          .add_actor_send<&conversation_spot_t::set_typing> (set_typing_msg_t::packet_name)
          .add_actor_request<&conversation_spot_t::close> (close_conversation_req_t::packet_name);
    }

    /* 공통 sample spec §14: 유휴 감지는 conversation Spot의 server-side timer가 소유한다.
     * idle deadline이 지나면 `WaitingForClose`로 바꾸고 idle 알림을, close grace가 지나면
     * 대화를 닫고 closed 알림을 **모든 참가자**에게 보낸다. */
    // --8<-- [start:doc-sc-idle-timer]
    task_t<void> on_initialize () override
    {
        _idle_timer = _context.add_timer<conversation_idle_timer_handler_t> (
          "conversation-idle", std::chrono::milliseconds (500));
        co_return;
    }

    task_t<void> on_idle_tick ()
    {
        if (!_conversation) {
            co_return;
        }
        auto transition = _conversation->advance_time (now_unix_ms ());
        if (const auto *idle = std::get_if<conversation_idle_notify_t> (&transition)) {
            report_status (idle->state);
            co_await broadcast (*idle, conversation_idle_notify_t::packet_name);
        }
        if (const auto *closed = std::get_if<conversation_closed_notify_t> (&transition)) {
            _runtime.release_conversation (closed->conversation_id);
            report_status (closed->state);
            co_await broadcast (*closed, conversation_closed_notify_t::packet_name);
        }
        co_return;
    }
    // --8<-- [end:doc-sc-idle-timer]

    task_t<spot_create_response_t> on_create (const zlink::framework::message_t &request) override
    {
        auto create = request.decode<conversation_create_req_t> ();
        _conversation = conversation_t (
          _context.spot_id (), create.subject, create.customer_actor_id);
        const auto state = _conversation->snapshot ();
        std::cout << "supportchat-conversation created conversation=" << state.conversation_id
                  << std::endl;
        report_status (state);
        co_return spot_create_response_t::accept (conversation_create_res_t{state});
    }

    task_t<spot_actor_join_result_t>
    on_actor_join (std::string_view actor_id, const zlink::framework::message_t &message) override
    {
        const auto profile = _runtime.actor_profile (std::string (actor_id));
        if (!profile) {
            co_return spot_actor_join_result_t::reject ();
        }
        const auto request = message.decode<join_conversation_req_t> ();
        const auto participant_id = profile->participant_id.empty () ? profile->actor_id
                                                                     : profile->participant_id;
        if (request.participant_id != participant_id || request.role != profile->role
            || request.display_name != profile->display_name) {
            co_return spot_actor_join_result_t::reject ();
        }
        // --8<-- [start:doc-sc-assign]
        auto projected = require_conversation ();
        conversation_state_t admission_state;
        if (request.role == role_t::agent) {
            admission_state = projected.join_agent (request.participant_id, request.display_name)
                                .state;
        } else {
            const auto joined = projected.join_customer (request.participant_id,
                                                         request.display_name);
            admission_state = joined.state;
            if (auto assigned = _runtime.assign_agent (joined.state.conversation_id)) {
                _pending_agent_assignments[std::string (actor_id)] = *assigned;
                admission_state = projected.assign_agent (*assigned).state;
            }
        }
        _pending_actor_joins.insert (std::string (actor_id));
        co_return spot_actor_join_result_t::accept (
          join_conversation_res_t{true, std::string (actor_id), admission_state});
        // --8<-- [end:doc-sc-assign]
    }

    task_t<void> on_actor_joined (support_user_actor_t &actor) override
    {
        const std::string actor_joined_begin_line = std::format (
          "supportchat conversation: actor_joined_begin actor={} role={}\n",
          actor.actor_id,
          actor.role);
        std::cerr << actor_joined_begin_line;
        if (_pending_actor_joins.erase (actor.actor_id) == 0) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "accepted support actor admission is missing");
        }
        (void) co_await join_actor (actor);
        const std::string actor_joined_complete_line = std::format (
          "supportchat conversation: actor_joined_complete actor={}\n", actor.actor_id);
        std::cerr << actor_joined_complete_line;
        co_return;
    }

    task_t<void> on_leave_actor (support_user_actor_t &) override { co_return; }

    task_t<join_conversation_res_t>
    join (support_user_actor_t &actor, message_context_t &, const join_conversation_req_t &request)
    {
        if (request.conversation_id != require_conversation ().snapshot ().conversation_id
            || request.participant_id != actor.participant_id || request.role != actor.role
            || request.display_name != actor.display_name) {
            throw framework_exception_t (framework_error_kind_t::rejected,
                                         "join does not match the conversation actor");
        }
        auto current = co_await join_actor (actor);
        current.scheduled = false;
        co_return current;
    }

    // --8<-- [start:doc-sc-message-push]
    task_t<send_chat_message_res_t> send_message (support_user_actor_t &actor,
                                                  message_context_t &,
                                                  const send_chat_message_req_t &request)
    {
        const auto previous = require_conversation ().snapshot ();
        auto sent = require_conversation ().send_message (
          actor.participant_id, request.text, now_unix_ms ());
        if (sent.state.status != previous.status) {
            report_status (sent.state);
        }
        co_await broadcast_except (
          actor.participant_id,
          chat_message_notify_t{sent.state.conversation_id, sent.message, sent.state},
          chat_message_notify_t::packet_name);
        co_return sent;
    }
    // --8<-- [end:doc-sc-message-push]

    task_t<void>
    set_typing (support_user_actor_t &actor, message_context_t &, const set_typing_msg_t &request)
    {
        if (require_conversation ().snapshot ().status == conversation_status_t::closed) {
            co_return;
        }
        auto typing = require_conversation ().set_typing (actor.participant_id, request.is_typing);
        if (auto peer = peer_for (actor.participant_id)) {
            co_await send_to_actor (*peer, typing, typing_changed_notify_t::packet_name);
        }
        co_return;
    }

    task_t<close_conversation_res_t> close (support_user_actor_t &actor,
                                            message_context_t &,
                                            const close_conversation_req_t &request)
    {
        (void) request;
        auto closed = require_conversation ().close ();
        _runtime.release_conversation (closed.state.conversation_id);
        report_status (closed.state);
        /* 종료 알림은 대화의 모든 참가자가 받는다(공통 sample spec §14). */
        co_await broadcast (
          conversation_closed_notify_t{closed.state.conversation_id, closed.state},
          conversation_closed_notify_t::packet_name);
        co_return close_conversation_res_t{closed.state};
    }

  private:
    static void report_status (const conversation_state_t &state)
    {
        std::cout << "supportchat-conversation status=" << state.status
                  << " conversation=" << state.conversation_id << std::endl;
    }

    task_t<join_conversation_res_t> join_actor (support_user_actor_t &actor)
    {
        if (actor.participant_id.empty ()) {
            actor.participant_id = actor.actor_id;
        }
        if (actor.role == role_t::agent) {
            auto joined = require_conversation ().join_agent (actor.participant_id,
                                                              actor.display_name);
            std::cout << "supportchat-conversation agent-joined conversation="
                      << joined.conversation_id << " agent=" << actor.participant_id << std::endl;
            report_status (joined.state);
            co_await broadcast (
              participant_joined_notify_t{
                joined.conversation_id, actor.participant_id, actor.role, joined.state},
              participant_joined_notify_t::packet_name);
            co_return join_conversation_res_t{false, actor.actor_id, joined.state};
        }

        auto joined = require_conversation ().join_customer (actor.participant_id,
                                                             actor.display_name);
        // --8<-- [start:doc-sc-roster-push]
        const auto pending = _pending_agent_assignments.find (actor.actor_id);
        if (pending != _pending_agent_assignments.end ()) {
            const auto assigned = pending->second;
            _pending_agent_assignments.erase (pending);
            auto assignment = require_conversation ().assign_agent (assigned);
            if (auto roster = _runtime.actor_for (assigned)) {
                co_await (*roster)
                  ->context ()
                  .bound_session ()
                  .send (conversation_assigned_notify_t{assignment.state.conversation_id,
                                                        assignment.state})
                  .async ();
            }
            co_return join_conversation_res_t{false, actor.actor_id, assignment.state};
        }
        // --8<-- [end:doc-sc-roster-push]
        co_return join_conversation_res_t{false, actor.actor_id, joined.state};
    }

    template <typename TMessage>
    task_t<void> broadcast_except (const std::string &excluded_participant_id,
                                   const TMessage &message,
                                   const char *packet_name)
    {
        const auto state = require_conversation ().snapshot ();
        if (state.customer_actor_id != excluded_participant_id) {
            co_await send_to_actor (state.customer_actor_id, message, packet_name);
        }
        if (state.agent_actor_id && !state.agent_actor_id->empty ()
            && *state.agent_actor_id != excluded_participant_id) {
            co_await send_to_actor (*state.agent_actor_id, message, packet_name);
        }
        co_return;
    }

    std::optional<std::string> peer_for (const std::string &participant_id) const
    {
        const auto state = require_conversation ().snapshot ();
        if (state.customer_actor_id != participant_id) {
            return state.customer_actor_id;
        }
        return state.agent_actor_id;
    }

    template <typename TMessage>
    task_t<void> broadcast (const TMessage &message, const char *packet_name)
    {
        const auto state = require_conversation ().snapshot ();
        co_await send_to_actor (state.customer_actor_id, message, packet_name);
        if (state.agent_actor_id && !state.agent_actor_id->empty ()) {
            co_await send_to_actor (*state.agent_actor_id, message, packet_name);
        }
        co_return;
    }

    static std::int64_t now_unix_ms ()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds> (
                 std::chrono::system_clock::now ().time_since_epoch ())
          .count ();
    }

    template <typename TMessage>
    task_t<void> send_to_actor (const std::string &participant_id,
                                const TMessage &message,
                                const char *packet_name)
    {
        auto actor = _runtime.actor_for (participant_id,
                                         require_conversation ().snapshot ().conversation_id);
        if (!actor) {
            const std::string missing_actor_line = std::format (
              "supportchat conversation: missing actor packet={} participant={}\n",
              packet_name,
              participant_id);
            std::cerr << missing_actor_line;
            co_return;
        }
        const std::string bound_push_begin_line = std::format (
          "supportchat conversation: bound_push_begin packet={} participant={}\n",
          packet_name,
          participant_id);
        std::cerr << bound_push_begin_line;
        co_await (*actor)->context ().bound_session ().send (message).async ();
        const std::string bound_push_line = std::format (
          "supportchat conversation: bound push packet={} participant={}\n",
          packet_name,
          participant_id);
        std::cerr << bound_push_line;
        co_return;
    }

    conversation_t &require_conversation ()
    {
        if (!_conversation) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "support conversation is not created");
        }
        return *_conversation;
    }

    const conversation_t &require_conversation () const
    {
        if (!_conversation) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "support conversation is not created");
        }
        return *_conversation;
    }

    supportchat_conversation_runtime_t &_runtime;
    spot_context_t _context;
    zlink::framework::timer_t _idle_timer;
    std::optional<conversation_t> _conversation;
    std::set<std::string> _pending_actor_joins;
    std::map<std::string, std::string> _pending_agent_assignments;
};

struct support_user_actor_factory_t final : public actor_factory_t<support_user_actor_t>
{
    task_t<std::shared_ptr<support_user_actor_t>> create (actor_context_t context,
                                                          std::stop_token) override
    {
        co_return std::make_shared<support_user_actor_t> (std::move (context));
    }
};

inline task_t<void>
conversation_idle_timer_handler_t::handle (conversation_spot_t &spot,
                                           const zlink::framework::timer_tick_t &) const
{
    co_await spot.on_idle_tick ();
}

class support_entry_spot_t : public entry_spot_t<support_user_actor_t>
{
  public:
    support_entry_spot_t (entry_spot_context_t context,
                          supportchat_conversation_runtime_t &runtime,
                          channel_client_t &channels) :
        _runtime (runtime), _context (std::move (context)), _channels (channels)
    {
    }

    entry_spot_context_t &context () noexcept override { return _context; }
    const entry_spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ()
          .add_actor_request<&support_entry_spot_t::set_available> (
            set_agent_available_req_t::packet_name)
          .add_actor_request<&support_entry_spot_t::join_conversation> (
            join_conversation_req_t::packet_name)
          .add_actor_request<&support_entry_spot_t::open_conversation> (
            open_conversation_req_t::packet_name);
    }

    task_t<actor_create_response_t>
    on_create_actor (support_user_actor_t &actor,
                     const zlink::framework::message_t &request) override
    {
        apply_actor_profile (actor, request.decode<ensure_support_user_actor_req_t> ());
        co_return actor_create_response_t::accept ();
    }

    task_t<void> on_actor_joined (support_user_actor_t &actor) override
    {
        const std::string actor_joined_begin_line = std::format (
          "supportchat support: actor_joined_begin actor={} role={}\n", actor.actor_id, actor.role);
        std::cerr << actor_joined_begin_line;
        _actors[actor.actor_id] = &actor;
        _runtime.remember_live_actor (actor);
        const std::string actor_joined_complete_line = std::format (
          "supportchat support: actor_joined_complete actor={} role={}\n",
          actor.actor_id,
          actor.role);
        std::cerr << actor_joined_complete_line;
        co_return;
    }

    task_t<void> on_leave_actor (support_user_actor_t &) override { co_return; }

    task_t<void> on_disconnect_actor (support_user_actor_t &actor) override
    {
        if (actor.role == role_t::agent) {
            (void) _runtime.set_agent_available (actor.actor_id, actor.display_name, false);
            const std::string line = std::format (
              "supportchat support: agent-disconnected actor={}\n", actor.actor_id);
            std::cerr << line;
        }
        co_return;
    }

    // --8<-- [start:doc-sc-set-available]
    set_agent_available_res_t set_available (support_user_actor_t &actor,
                                             message_context_t &,
                                             const set_agent_available_req_t &request)
    {
        if (actor.role != role_t::agent) {
            throw framework_exception_t (framework_error_kind_t::rejected,
                                         "only agent actors can set availability");
        }
        return _runtime.set_agent_available (
          actor.actor_id, actor.display_name, request.is_available);
    }
    // --8<-- [end:doc-sc-set-available]

    join_conversation_res_t join_conversation (support_user_actor_t &actor,
                                               message_context_t &,
                                               const join_conversation_req_t &request)
    {
        if (request.conversation_id.empty ()) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "JoinConversationReq is missing conversationId");
        }
        if (request.participant_id != actor.participant_id || request.role != actor.role
            || request.display_name != actor.display_name) {
            throw framework_exception_t (framework_error_kind_t::rejected,
                                         "join participant does not match the bound actor");
        }
        return actor.schedule_conversation_join (request.conversation_id,
                                                 actor.role == role_t::agent);
    }

    // --8<-- [start:doc-sc-open-actor]
    task_t<open_conversation_res_t> open_conversation (support_user_actor_t &actor,
                                                       message_context_t &,
                                                       const open_conversation_req_t &request)
    {
        if (actor.role != role_t::customer) {
            throw framework_exception_t (framework_error_kind_t::rejected,
                                         "only customer actors can open conversations");
        }
        auto allocated = co_await _channels
                           .request ("supportchat.api",
                                     open_conversation_api_req_t{
                                       actor.actor_id, actor.display_name, request.subject})
                           .async<open_conversation_api_res_t> ();
        const auto conversation_id = allocated.state.conversation_id;
        auto scheduled = actor.schedule_conversation_join (conversation_id, false);
        co_return open_conversation_res_t{conversation_id, std::move (scheduled.state)};
    }
    // --8<-- [end:doc-sc-open-actor]

  private:
    void apply_actor_profile (support_user_actor_t &actor, ensure_support_user_actor_req_t profile)
    {
        actor.display_name = std::move (profile.display_name);
        actor.role = std::move (profile.role);
        actor.participant_id = std::move (profile.participant_id);
        if (actor.participant_id.empty ()) {
            actor.participant_id = actor.actor_id;
        }
        _actors[actor.actor_id] = &actor;
        _runtime.remember_actor (
          actor.actor_id, actor.display_name, actor.role, actor.participant_id);
        _runtime.remember_live_actor (actor);
    }

    template <typename TMessage>
    void
    send_to_actor (const std::string &actor_id, const TMessage &message, const char *packet_name)
    {
        const auto found = _actors.find (actor_id);
        if (found == _actors.end ()) {
            const std::string missing_actor_line = std::format (
              "supportchat support: missing actor packet={} actor={}\n", packet_name, actor_id);
            std::cerr << missing_actor_line;
            return;
        }
        found->second->context ().bound_session ().send (message).async ();
        const std::string bound_push_line = std::format (
          "supportchat support: bound push packet={} actor={}\n", packet_name, actor_id);
        std::cerr << bound_push_line;
    }

    supportchat_conversation_runtime_t &_runtime;
    entry_spot_context_t _context;
    channel_client_t &_channels;
    std::map<std::string, support_user_actor_t *> _actors;
};

class ensure_support_user_actor_handler_t
{
  public:
    using request_type = ensure_support_user_actor_req_t;
    using reply_type = ensure_support_user_actor_res_t;
    static constexpr const char *topic_name = "EnsureSupportUserActorReq";

    explicit ensure_support_user_actor_handler_t (actor_manager_t &actors) : _actors (actors) {}

    task_t<ensure_support_user_actor_res_t> handle (const ensure_support_user_actor_req_t &request)
    {
        auto created = co_await _actors
                         .get_or_create (actor_id_t (request.actor_id), support_user_actor_type)
                         .in_mesh (sample_names_t::mesh)
                         .creation_request (request)
                         .async ();
        if (const auto *existing = std::get_if<actor_create_existing_t> (&created))
            co_return ensure_support_user_actor_res_t{actor_location_t::from (existing->actor)};
        if (const auto *actor = std::get_if<actor_create_created_t> (&created))
            co_return ensure_support_user_actor_res_t{actor_location_t::from (actor->actor)};
        throw framework_exception_t (framework_error_kind_t::rejected,
                                     "support actor creation was rejected");
    }

  private:
    actor_manager_t &_actors;
};

class ensure_agent_conversation_handler_t
{
  public:
    using request_type = ensure_agent_conversation_req_t;
    using reply_type = ensure_agent_conversation_res_t;
    static constexpr const char *topic_name = "EnsureAgentConversationReq";

    explicit ensure_agent_conversation_handler_t (actor_manager_t &actors) : _actors (actors) {}

    task_t<ensure_agent_conversation_res_t> handle (const ensure_agent_conversation_req_t &request)
    {
        const auto
          conversation_actor_id = supportchat_conversation_runtime_t::conversation_actor_id (
            request.roster_actor_id, request.conversation_id);
        auto created = co_await _actors
                         .get_or_create (actor_id_t (conversation_actor_id),
                                         support_user_actor_type)
                         .in_mesh (sample_names_t::mesh)
                         .creation_request (
                           ensure_support_user_actor_req_t{conversation_actor_id,
                                                           request.display_name,
                                                           role_t::agent,
                                                           request.roster_actor_id})
                         .async ();
        std::optional<actor_ref_t> actor;
        if (const auto *existing = std::get_if<actor_create_existing_t> (&created))
            actor = existing->actor;
        else if (const auto *materialized = std::get_if<actor_create_created_t> (&created))
            actor = materialized->actor;
        else
            throw framework_exception_t (framework_error_kind_t::rejected,
                                         "support conversation actor creation was rejected");
        co_return ensure_agent_conversation_res_t{actor_location_t::from (*actor)};
    }

  private:
    actor_manager_t &_actors;
};

} // namespace zlink::samples::supportchat

int main (int argc, char **argv)
{
    using namespace zlink::framework;
    using namespace zlink::samples::supportchat;

    auto app = app_t::create ();
    const auto configuration = load_sample_configuration (app, argc, argv);
    const auto &topology = configuration.topology;
    std::filesystem::create_directories (configuration.role.log_dir);

    app.logging ().use_file (configuration.flow_log_path ());
    auto &options = app.add_zlink_framework ();
    options.configure_dispatch ().message_flow (message_flow_log_mode_t::normal);
    options.add_location_store<redis::redis_location_store_t> ()
      .set_connection_string (topology.redis_endpoint)
      .set_key_prefix (topology.redis_key_prefix + "location:");
    options.add_relocation_store<redis::redis_relocation_store_t> ()
      .set_connection_string (topology.redis_endpoint)
      .set_key_prefix (topology.redis_key_prefix + "relocation:");
    options.services ().add_singleton<supportchat_conversation_runtime_t> ();
    options.add_client_server_channel ("supportchat.support")
      .server ()
      .set_bind_host (host_from_tcp_endpoint (topology.support_route_endpoint))
      .set_advertise_host (host_from_tcp_endpoint (topology.support_route_endpoint))
      .listen (port_from_tcp_endpoint (topology.support_route_endpoint))
      .add_handler_group ("supportchat-support");
    options.add_client_server_channel ("supportchat.api").client ();
    options.handlers ()
      .group ("supportchat-support")
      .add<ensure_support_user_actor_handler_t> ()
      .add<ensure_agent_conversation_handler_t> ();
    options.http ().listen (topology.support_http_url).map_health ("/health");
    // --8<-- [start:doc-sc-support-register]
    auto support_spot = options.add_route_mesh (sample_names_t::mesh);
    support_spot.set_routing_id (
      zlink::routing_id_t::from (sample_names_t::support_node_routing_id));
    support_spot.listen (topology.support_spot_router_endpoint);
    support_spot.objects ()
      .server ()
      .add_entry_spot<support_entry_spot_t, supportchat_conversation_runtime_t, channel_client_t> ()
      .add_spot_factory<conversation_spot_t, supportchat_conversation_runtime_t> (
        sample_names_t::conversation_spot)
      .disable_relocation ()
      .add_actor_factory<support_user_actor_t, support_user_actor_factory_t> (
        support_user_actor_type)
      .preserve_state_with<support_user_actor_relocation_adapter_t> ();
    // --8<-- [end:doc-sc-support-register]
    app.add_hosted_service (std::make_unique<sample_readiness_service_t> ("public", "support"));
    return app.run (argc, argv);
}
