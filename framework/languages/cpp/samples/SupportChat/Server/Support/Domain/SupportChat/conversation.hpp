/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../../Shared/Contracts/messages.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace zlink::samples::supportchat
{

struct conversation_participant_t
{
    std::string actor_id;
    std::string role;
    std::string display_name;
    bool is_typing{false};
};

struct conversation_event_t
{
    std::string packet_name;
    std::string actor_id;
    bool is_typing{false};
    std::optional<chat_message_t> message;
    conversation_state_t state;
};

using conversation_timeout_event_t =
  std::variant<std::monostate, conversation_idle_notify_t, conversation_closed_notify_t>;

class conversation_t
{
  public:
    conversation_t (std::string conversation_id, std::string subject, std::string customer_actor_id) :
        _conversation_id (std::move (conversation_id)),
        _subject (std::move (subject)),
        _customer_actor_id (std::move (customer_actor_id)),
        _status (conversation_status_t::waiting_for_agent)
    {
    }

    participant_joined_notify_t join_customer (const std::string &actor_id,
                                               const std::string &display_name)
    {
        ensure_open ("join customer");
        upsert_participant ({actor_id, role_t::customer, display_name, false});
        return {_conversation_id, actor_id, role_t::customer, snapshot ()};
    }

    conversation_assigned_notify_t assign_agent (const std::string &roster_actor_id)
    {
        ensure_open ("assign agent");
        _agent_actor_id = roster_actor_id;
        return {_conversation_id, snapshot ()};
    }

    participant_joined_notify_t join_agent (const std::string &roster_actor_id,
                                            const std::string &display_name)
    {
        ensure_open ("join agent");
        _agent_actor_id = roster_actor_id;
        _status = conversation_status_t::active;
        upsert_participant ({roster_actor_id, role_t::agent, display_name, false});
        return {_conversation_id, roster_actor_id, role_t::agent, snapshot ()};
    }

    send_chat_message_res_t send_message (const std::string &sender_actor_id,
                                          const std::string &text,
                                          std::int64_t now_unix_ms)
    {
        ensure_participant (sender_actor_id);
        if (_status == conversation_status_t::closed) {
            throw std::logic_error ("closed conversation cannot accept chat messages");
        }
        if (_status == conversation_status_t::waiting_for_close) {
            _status = conversation_status_t::active;
            _close_deadline_unix_ms.reset ();
        }
        auto message = chat_message_t{_conversation_id,
                                      ++_last_message_seq,
                                      sender_actor_id,
                                      text,
                                      now_unix_ms};
        _last_message_at_unix_ms = now_unix_ms;
        _idle_deadline_unix_ms = now_unix_ms + idle_timeout_ms;
        return {message, snapshot ()};
    }

    typing_changed_notify_t set_typing (const std::string &sender_actor_id, bool is_typing)
    {
        if (_status == conversation_status_t::closed) {
            return {_conversation_id, sender_actor_id, is_typing, snapshot ()};
        }
        ensure_participant (sender_actor_id);
        for (auto &participant : _participants) {
            if (participant.actor_id == sender_actor_id) {
                participant.is_typing = is_typing;
                break;
            }
        }
        return {_conversation_id, sender_actor_id, is_typing, snapshot ()};
    }

    conversation_timeout_event_t advance_time (std::int64_t now_unix_ms)
    {
        if (_status == conversation_status_t::closed) {
            return std::monostate{};
        }
        if (_status == conversation_status_t::active && _idle_deadline_unix_ms
            && now_unix_ms >= *_idle_deadline_unix_ms) {
            _status = conversation_status_t::waiting_for_close;
            _close_deadline_unix_ms = now_unix_ms + close_grace_ms;
            return conversation_idle_notify_t{_conversation_id, snapshot ()};
        }
        if (_status == conversation_status_t::waiting_for_close && _close_deadline_unix_ms
            && now_unix_ms >= *_close_deadline_unix_ms) {
            _status = conversation_status_t::closed;
            _close_deadline_unix_ms.reset ();
            return conversation_closed_notify_t{_conversation_id, snapshot ()};
        }
        return std::monostate{};
    }

    /* 이미 닫힌 대화를 다시 닫는 것은 오류다(공통 sample spec §9, §17 명시적 close 시나리오). */
    conversation_closed_notify_t close ()
    {
        ensure_open ("close");
        _status = conversation_status_t::closed;
        _close_deadline_unix_ms.reset ();
        return {_conversation_id, snapshot ()};
    }

    conversation_state_t snapshot () const
    {
        return {_conversation_id,
                _subject,
                _status,
                _customer_actor_id,
                _agent_actor_id,
                _last_message_seq,
                _last_message_at_unix_ms,
                _idle_deadline_unix_ms};
    }

    static constexpr std::int64_t idle_timeout_ms = 3000;
    static constexpr std::int64_t close_grace_ms = 2000;

  private:
    void ensure_open (const std::string &action) const
    {
        if (_status == conversation_status_t::closed) {
            throw std::logic_error ("cannot " + action + " on closed conversation");
        }
    }

    void ensure_participant (const std::string &actor_id) const
    {
        const auto found = std::any_of (_participants.begin (), _participants.end (),
                                        [&] (const auto &participant) {
                                            return participant.actor_id == actor_id;
                                        });
        if (!found) {
            throw std::logic_error ("actor is not a conversation participant: " + actor_id);
        }
    }

    void upsert_participant (conversation_participant_t participant)
    {
        const auto existing = std::find_if (_participants.begin (), _participants.end (),
                                           [&] (const auto &current) {
                                               return current.actor_id == participant.actor_id;
                                           });
        if (existing == _participants.end ()) {
            _participants.push_back (std::move (participant));
            return;
        }
        *existing = std::move (participant);
    }

    std::string _conversation_id;
    std::string _subject;
    std::string _customer_actor_id;
    std::optional<std::string> _agent_actor_id;
    std::string _status;
    std::uint64_t _last_message_seq{0};
    std::optional<std::int64_t> _last_message_at_unix_ms;
    std::optional<std::int64_t> _idle_deadline_unix_ms;
    std::optional<std::int64_t> _close_deadline_unix_ms;
    std::vector<conversation_participant_t> _participants;
};

} // namespace zlink::samples::supportchat
