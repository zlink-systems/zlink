/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Shared/Contracts/messages.hpp"

#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/stream_connector.hpp>
#include <zlink/stream_e2e_client.hpp>
#include <zlink/stream_e2e_client/codecs/auto_codec.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <functional>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::samples::supportchat
{

inline constexpr const char *conversation_id_metadata_key = "ConversationId";

class supportchat_client_scenario_t
{
  public:
    /* 공통 sample spec §1: client는 Session stream 하나만 사용한다. 서버 내부 불변식
     * 상담원 가용성과 대화 순서는 실제 request·push 흐름의 결과로 검증한다. */
    void run (const std::string &session_stream_endpoint)
    {
        run_stream_conversation (session_stream_endpoint);
    }

  private:
    static constexpr auto notification_wait_timeout = std::chrono::seconds (20);

    using connector_t = zlink::stream_e2e_client::coroutine_connector_t;

    static zlink::stream_connector::connector_t make_connector (const std::string &endpoint)
    {
        zlink::stream_connector::connector_options_t options;
        options.endpoint = endpoint;
        options.connect_timeout = std::chrono::seconds (5);
        options.request_timeout = std::chrono::seconds (12);
        options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        return zlink::stream_connector::connector_factory_t::create (options);
    }

    static void run_stream_conversation (const std::string &endpoint)
    {
        auto customer_core = make_connector (endpoint);
        auto customer = zlink::stream_e2e_client::use (customer_core);
        auto customer_connected = customer.connect ().submit ();
        expect (static_cast<bool> (customer_connected), "customer stream connect failed");

        auto agent_core = make_connector (endpoint);
        auto agent = zlink::stream_e2e_client::use (agent_core);
        auto agent_connected = agent.connect ().submit ();
        expect (static_cast<bool> (agent_connected), "agent stream connect failed");

        auto customer_auth = request<authenticate_res_t> (
          customer, authenticate_req_t{"customer"}, "customer auth failed");
        auto agent_auth =
          request<authenticate_res_t> (agent, authenticate_req_t{"agent"}, "agent auth failed");
        expect (customer_auth.role == role_t::customer, "customer role mismatch");
        expect (agent_auth.role == role_t::agent, "agent role mismatch");
        std::cout << "supportchat authentication=verified" << std::endl;

        auto customer_availability =
          customer.request (set_agent_available_req_t{true})
            .async<set_agent_available_res_t> ()
            .result ();
        expect (!customer_availability, "customer must not set agent availability");

        auto available = request<set_agent_available_res_t> (
          agent, set_agent_available_req_t{true}, "agent availability failed");
        expect (available.is_available, "agent was not made available");

        auto assigned = wait_assigned_packet (agent);
        auto opened = request<open_conversation_res_t> (
          customer, open_conversation_req_t{"checkout payment failed"}, "open conversation failed");
        expect (opened.state.status == conversation_status_t::waiting_for_agent,
                "new conversation must remain waiting until the agent joins");
        expect (opened.state.conversation_id == opened.conversation_id,
                "open response conversation id mismatch");
        expect_public_contract (opened, "open conversation response");
        expect (assigned.get ().conversation_id == opened.conversation_id,
                "assignment notification mismatch");
        std::cout << "supportchat conversation-assignment=verified" << std::endl;

        auto joined_customer = wait_joined (
          customer, opened.conversation_id, "customer participant join wait failed");
        auto joined_agent = wait_joined (
          agent, opened.conversation_id, "agent participant join wait failed");
        auto agent_joined = request_in_conversation<join_conversation_res_t> (
          agent, opened.conversation_id,
          join_conversation_req_t{"agent-1", role_t::agent, "Agent One"}, "agent join failed");
        expect (agent_joined.scheduled,
                "agent join must report a deferred membership operation");
        expect (agent_joined.state.status == conversation_status_t::waiting_for_agent,
                "scheduled agent join must return the pre-commit state");
        const auto customer_joined_notify = joined_customer.get ();
        const auto agent_joined_notify = joined_agent.get ();
        expect (customer_joined_notify.actor_id == "agent-1"
                  && customer_joined_notify.state.status == conversation_status_t::active,
                "customer did not receive an active participant notification");
        expect (agent_joined_notify.actor_id == "agent-1"
                  && agent_joined_notify.state.status == conversation_status_t::active,
                "agent did not receive an active participant notification");
        expect (!customer_joined_notify.state.subject.empty ()
                  && customer_joined_notify.state.subject == agent_joined_notify.state.subject,
                "participant notification subject mismatch");
        const auto conversation_subject = customer_joined_notify.state.subject;
        expect_public_contract (customer_joined_notify, "customer participant notification");
        expect_public_contract (agent_joined_notify, "agent participant notification");

        auto greeting_for_customer = wait_chat (
          customer, opened.conversation_id, "How can I help?");
        auto greeting = request_in_conversation<send_chat_message_res_t> (
          agent, opened.conversation_id, send_chat_message_req_t{"How can I help?"},
          "agent greeting failed");
        expect (greeting.message.message_seq == 1
                  && greeting.state.status == conversation_status_t::active,
                "agent greeting sequence mismatch");
        expect (greeting.message.sent_at_unix_ms > 0, "greeting timestamp must be positive");
        const auto greeting_notify = greeting_for_customer.get ();
        expect (greeting_notify.message.message_seq == 1
                  && greeting_notify.message.sender_actor_id == "agent-1"
                  && greeting_notify.state.subject == conversation_subject,
                "customer did not receive agent greeting");
        expect_public_contract (greeting, "agent greeting response");
        expect_public_contract (greeting_notify, "customer greeting notification");
        std::cout << "supportchat bound-push=verified" << std::endl;

        auto reply_for_agent = wait_chat (
          agent, opened.conversation_id, "Payment keeps failing.");
        auto reply = request_in_conversation<send_chat_message_res_t> (
          customer, opened.conversation_id, send_chat_message_req_t{"Payment keeps failing."},
          "customer reply failed");
        expect (reply.message.message_seq == 2
                  && reply.state.status == conversation_status_t::active,
                "customer reply sequence mismatch");
        const auto reply_notify = reply_for_agent.get ();
        expect (reply_notify.message.message_seq == 2
                  && reply_notify.message.sender_actor_id == "customer-1",
                "agent did not receive customer reply");
        expect_public_contract (reply, "customer reply response");
        expect_public_contract (reply_notify, "agent reply notification");

        /* 같은 상담원이 용량 안에서 두 번째 방을 받는다(공통 sample spec §17-13~17). */
        auto second_customer_core = make_connector (endpoint);
        auto second_customer = zlink::stream_e2e_client::use (second_customer_core);
        expect (static_cast<bool> (second_customer.connect ().submit ()),
                "second customer stream connect failed");
        auto second_auth = request<authenticate_res_t> (
          second_customer, authenticate_req_t{"customer-2"}, "second customer auth failed");
        expect (second_auth.role == role_t::customer, "second customer role mismatch");

        auto second_assigned = wait_assigned_packet (agent);

        auto second_opened =
          request<open_conversation_res_t> (second_customer,
                                            open_conversation_req_t{"refund not received"},
                                            "second open conversation failed");
        expect (second_opened.conversation_id != opened.conversation_id,
                "second conversation must have its own id");
        expect (second_assigned.get ().conversation_id == second_opened.conversation_id,
                "second assignment notification mismatch");

        auto second_joined_customer = wait_joined (
          second_customer, second_opened.conversation_id,
          "second customer participant join wait failed");
        auto second_joined_agent = wait_joined (
          agent, second_opened.conversation_id, "second agent participant join wait failed");
        auto second_agent_joined = request_in_conversation<join_conversation_res_t> (
          agent, second_opened.conversation_id,
          join_conversation_req_t{"agent-1", role_t::agent, "Agent One"},
          "agent join of second conversation failed");
        expect (second_agent_joined.scheduled,
                "second agent join must report a deferred membership operation");
        expect (second_agent_joined.state.status == conversation_status_t::waiting_for_agent,
                "second scheduled join must return the pre-commit state");
        expect (second_joined_customer.get ().state.status == conversation_status_t::active,
                "second customer did not receive active participant notification");
        expect (second_joined_agent.get ().state.subject == "refund not received",
                "second agent participant notification subject mismatch");

        auto second_greeting_for_customer = wait_chat (
          second_customer, second_opened.conversation_id, "Let me check your account.");
        auto second_greeting = request_in_conversation<send_chat_message_res_t> (
          agent, second_opened.conversation_id,
          send_chat_message_req_t{"Let me check your account."}, "second conversation greeting failed");
        expect (second_greeting.message.message_seq == 1
                  && second_greeting.state.conversation_id == second_opened.conversation_id,
                "second conversation sequence must start at 1");
        expect (second_greeting_for_customer.get ().message.message_seq == 1,
                "second customer did not receive agent greeting");

        auto customer_typing = wait_typing (
          customer, opened.conversation_id, "agent-1", true);
        auto agent_typing_none = wait_no_typing (agent);
        agent.send (set_typing_msg_t{true})
          .metadata (conversation_id_metadata_key, opened.conversation_id)
          .submit ();
        expect (customer_typing.get ().is_typing, "customer did not receive typing notification");
        agent_typing_none.get ();

        /* Keep the first conversation active while the second conversation and
         * reconnect assertions run. This is a real domain message, so the
         * reconnect check observes the latest committed sequence. */
        auto reconnect_keepalive_for_agent = wait_chat (
          agent, opened.conversation_id, "Still looking into it.");
        auto reconnect_keepalive = request_in_conversation<send_chat_message_res_t> (
          customer, opened.conversation_id, send_chat_message_req_t{"Still looking into it."},
          "reconnect keepalive failed");
        expect (reconnect_keepalive.message.message_seq == 3
                  && reconnect_keepalive.state.status == conversation_status_t::active,
                "reconnect keepalive sequence mismatch");
        expect (reconnect_keepalive_for_agent.get ().message.message_seq == 3,
                "agent did not receive reconnect keepalive");

        /* reconnect: 같은 token으로 새 stream을 열고 열려 있던 방에 다시 join한다(§15, §17-19~20). */
        expect (static_cast<bool> (customer_core.close ()), "customer disconnect failed");
        auto reconnected_customer_core = make_connector (endpoint);
        auto reconnected_customer = zlink::stream_e2e_client::use (reconnected_customer_core);
        expect (static_cast<bool> (reconnected_customer.connect ().submit ()),
                "customer reconnect failed");
        auto reconnected_customer_auth = request<authenticate_res_t> (
          reconnected_customer, authenticate_req_t{"customer"},
          "customer re-authentication failed");
        expect (reconnected_customer_auth.actor_id == customer_auth.actor_id,
                "reconnected customer must bind the same actor");
        auto customer_rejoined = request_in_conversation<join_conversation_res_t> (
          reconnected_customer, opened.conversation_id,
          join_conversation_req_t{"customer-1", role_t::customer, "Customer One"},
          "reconnected customer could not re-join the first conversation");
        expect (!customer_rejoined.scheduled
                  && customer_rejoined.state.status == conversation_status_t::active
                  && customer_rejoined.state.subject == conversation_subject
                  && customer_rejoined.state.last_message_seq == 3,
                "customer reconnect did not preserve conversation state");

        expect (static_cast<bool> (agent_core.close ()), "agent disconnect failed");
        auto reconnected_core = make_connector (endpoint);
        auto reconnected_agent = zlink::stream_e2e_client::use (reconnected_core);
        expect (static_cast<bool> (reconnected_agent.connect ().submit ()),
                "agent reconnect failed");
        auto reconnected_auth = request<authenticate_res_t> (
          reconnected_agent, authenticate_req_t{"agent"}, "agent re-authentication failed");
        expect (reconnected_auth.actor_id == agent_auth.actor_id,
                "reconnected agent must bind the same actor");
        auto reconnected_available = request<set_agent_available_res_t> (
          reconnected_agent, set_agent_available_req_t{true}, "agent re-availability failed");
        expect (reconnected_available.is_available, "reconnected agent was not made available");

        auto rejoined_first = request_in_conversation<join_conversation_res_t> (
          reconnected_agent, opened.conversation_id,
          join_conversation_req_t{"agent-1", role_t::agent, "Agent One"},
          "reconnected agent could not re-join the first conversation");
        expect (!rejoined_first.scheduled,
                "reconnect must not schedule an already committed Join");
        expect (rejoined_first.state.status == conversation_status_t::active,
                "first conversation state must survive the reconnect");
        expect (rejoined_first.state.subject == conversation_subject
                  && rejoined_first.state.last_message_seq == 3,
                "first conversation history must survive the reconnect");
        auto rejoined_second = request_in_conversation<join_conversation_res_t> (
          reconnected_agent, second_opened.conversation_id,
          join_conversation_req_t{"agent-1", role_t::agent, "Agent One"},
          "reconnected agent could not re-join the second conversation");
        expect (!rejoined_second.scheduled,
                "second reconnect must return current state");
        expect (rejoined_second.state.last_message_seq == 1,
                "second conversation history must survive the reconnect");
        std::cout << "supportchat reconnect=verified" << std::endl;

        /* Register both idle waits before the timer can publish. The first idle
         * transition is resumed within the grace window, then the next idle
         * transition is allowed to close the conversation. */
        auto first_idle_customer = wait_idle (
          reconnected_customer, opened.conversation_id,
          "customer idle notification wait failed");
        auto first_idle_agent = wait_idle (
          reconnected_agent, opened.conversation_id,
          "agent idle notification wait failed");

        /* 명시적 close와 closed 대화 오류(§17-22, 명시적 close 시나리오). */
        auto second_closed_notify = wait_closed (
          reconnected_agent, second_opened.conversation_id,
          "reconnected agent second conversation closed notification wait failed");
        auto closed = request_in_conversation<close_conversation_res_t> (
          second_customer, second_opened.conversation_id, close_conversation_req_t{"resolved"},
          "explicit close failed");
        expect (closed.state.status == conversation_status_t::closed,
                "explicit close did not close the conversation");
        expect (second_closed_notify.get ().state.status == conversation_status_t::closed,
                "agent did not receive the closed notification");

        (void) zlink::stream_connector::assertions::expect_failure ([&] {
            return second_customer.request (close_conversation_req_t{"resolved"})
              .metadata (conversation_id_metadata_key, second_opened.conversation_id)
              .async<close_conversation_res_t> ()
              .result ();
        });
        (void) zlink::stream_connector::assertions::expect_failure ([&] {
            return second_customer.request (send_chat_message_req_t{"anyone there?"})
              .metadata (conversation_id_metadata_key, second_opened.conversation_id)
              .async<send_chat_message_res_t> ()
              .result ();
        });
        auto closed_typing_none = wait_no_typing (reconnected_agent);
        second_customer.send (set_typing_msg_t{true})
          .metadata (conversation_id_metadata_key, second_opened.conversation_id)
          .submit ();
        closed_typing_none.get ();
        std::cout << "supportchat-closed-typing-ignore=verified" << std::endl;

        expect (first_idle_customer.get ().state.status == conversation_status_t::waiting_for_close,
                "customer did not receive idle notification");
        expect (first_idle_agent.get ().state.status == conversation_status_t::waiting_for_close,
                "agent did not receive idle notification");
        auto resumed_for_agent = wait_chat (
          reconnected_agent, opened.conversation_id, "The customer resumed the conversation.");
        auto resumed = request_in_conversation<send_chat_message_res_t> (
          reconnected_customer, opened.conversation_id,
          send_chat_message_req_t{"The customer resumed the conversation."},
          "idle conversation resume failed");
        expect (resumed.message.message_seq == 4
                  && resumed.state.status == conversation_status_t::active,
                "idle conversation did not resume within grace");
        expect (resumed_for_agent.get ().state.status == conversation_status_t::active,
                "agent did not receive resumed conversation message");
        std::cout << "supportchat idle-resume=verified" << std::endl;

        auto second_idle_customer = wait_idle (
          reconnected_customer, opened.conversation_id,
          "resumed customer idle notification wait failed");
        auto second_idle_agent = wait_idle (
          reconnected_agent, opened.conversation_id,
          "resumed agent idle notification wait failed");
        auto first_closed_customer = wait_closed (
          reconnected_customer, opened.conversation_id,
          "customer closed notification wait failed");
        auto first_closed_agent = wait_closed (
          reconnected_agent, opened.conversation_id,
          "agent closed notification wait failed");
        expect (second_idle_customer.get ().state.status == conversation_status_t::waiting_for_close,
                "resumed customer did not receive idle notification");
        expect (second_idle_agent.get ().state.status == conversation_status_t::waiting_for_close,
                "resumed agent did not receive idle notification");
        expect (first_closed_customer.get ().state.status == conversation_status_t::closed,
                "customer did not receive closed notification");
        expect (first_closed_agent.get ().state.status == conversation_status_t::closed,
                "agent did not receive closed notification");
        std::cout << "supportchat idle-close=verified" << std::endl;

        (void) zlink::stream_connector::assertions::expect_failure ([&] {
            return reconnected_customer.request (send_chat_message_req_t{"are you there?"})
              .metadata (conversation_id_metadata_key, opened.conversation_id)
              .async<send_chat_message_res_t> ()
              .result ();
        });

        auto unavailable = request<set_agent_available_res_t> (
          reconnected_agent, set_agent_available_req_t{false},
          "agent availability disable failed");
        expect (!unavailable.is_available, "agent remained available");
        auto waiting_customer_core = make_connector (endpoint);
        auto waiting_customer = zlink::stream_e2e_client::use (waiting_customer_core);
        expect (static_cast<bool> (waiting_customer.connect ().submit ()),
                "waiting customer stream connect failed");
        auto waiting_auth = request<authenticate_res_t> (
          waiting_customer, authenticate_req_t{"customer-3"}, "waiting customer auth failed");
        expect (waiting_auth.actor_id == "customer-3", "waiting customer actor mismatch");
        auto no_agent_open = request<open_conversation_res_t> (
          waiting_customer, open_conversation_req_t{"agent unavailable"},
          "no-agent conversation open failed");
        expect (no_agent_open.state.status == conversation_status_t::waiting_for_agent
                  && no_agent_open.state.conversation_id == no_agent_open.conversation_id,
                "no-agent conversation did not remain waiting");
        expect_public_contract (no_agent_open, "no-agent open response");
        std::cout << "supportchat=completed" << std::endl;
    }

    static std::future<conversation_idle_notify_t> wait_idle (connector_t &connector,
                                                              std::string conversation_id,
                                                              const char *failure_message)
    {
        return connector.wait_for<conversation_idle_notify_t> ()
          .where (&conversation_idle_notify_t::conversation_id, std::move (conversation_id))
          .timeout (notification_wait_timeout)
          .to_future (failure_message);
    }

    static std::future<conversation_closed_notify_t> wait_closed (connector_t &connector,
                                                                  std::string conversation_id,
                                                                  const char *failure_message)
    {
        return connector.wait_for<conversation_closed_notify_t> ()
          .where (&conversation_closed_notify_t::conversation_id, std::move (conversation_id))
          .timeout (notification_wait_timeout)
          .to_future (failure_message);
    }

    template <typename TReply, typename TRequest>
    static TReply request (connector_t &connector, const TRequest &request, const char *message)
    {
        auto reply = connector.request (request).template async<TReply> ().result ();
        if (!reply) {
            throw std::runtime_error (reply.error () ? reply.error ()->message : message);
        }
        return reply.value ();
    }

    template <typename TReply, typename TRequest>
    static TReply request_in_conversation (connector_t &connector,
                                           const std::string &conversation_id,
                                           const TRequest &request,
                                           const char *message)
    {
        auto reply = connector.request (request)
                       .metadata (conversation_id_metadata_key, conversation_id)
                       .template async<TReply> ()
                       .result ();
        if (!reply) {
            throw std::runtime_error (reply.error () ? reply.error ()->message : message);
        }
        return reply.value ();
    }

    static std::future<conversation_assigned_notify_t> wait_assigned_packet (connector_t &agent)
    {
        return std::async (std::launch::async, [&agent] {
            return agent.wait_for<conversation_assigned_notify_t> ()
              .timeout (std::chrono::seconds (12))
              .to_future ("assignment packet wait failed")
              .get ();
        });
    }

    static std::future<participant_joined_notify_t> wait_joined (
      connector_t &connector,
      std::string conversation_id,
      const char *failure_message)
    {
        return connector.wait_for<participant_joined_notify_t> ()
          .where (&participant_joined_notify_t::conversation_id, std::move (conversation_id))
          .timeout (std::chrono::seconds (12))
          .to_future (failure_message);
    }

    static std::future<chat_message_notify_t> wait_chat (connector_t &connector,
                                                         std::string conversation_id,
                                                         std::string text)
    {
        return connector.wait_for<chat_message_notify_t> ()
          .where ([conversation_id = std::move (conversation_id), text = std::move (text)] (
                    const chat_message_notify_t &message) {
              return message.conversation_id == conversation_id && message.message.text == text;
          })
          .timeout (std::chrono::seconds (12))
          .to_future ("chat message wait failed");
    }

    static std::future<typing_changed_notify_t> wait_typing (connector_t &connector,
                                                              std::string conversation_id,
                                                              std::string actor_id,
                                                              bool is_typing)
    {
        return connector.wait_for<typing_changed_notify_t> ()
          .where ([conversation_id = std::move (conversation_id), actor_id = std::move (actor_id),
                    is_typing] (const typing_changed_notify_t &message) {
              return message.conversation_id == conversation_id && message.actor_id == actor_id
                     && message.is_typing == is_typing;
          })
          .timeout (std::chrono::seconds (12))
          .to_future ("typing wait failed");
    }

    static std::future<void> wait_no_typing (connector_t &connector)
    {
        return std::async (std::launch::async, [&connector] {
            const auto result = connector.expect_none<typing_changed_notify_t> ()
                                  .within (std::chrono::milliseconds (500))
                                  .submit ();
            if (!result) {
                throw std::runtime_error ("unexpected typing notification");
            }
        });
    }

    template <typename TMessage>
    static void expect_public_contract (const TMessage &message, const char *context)
    {
        const nlohmann::json wire = message;
        static constexpr std::array<std::string_view, 10> forbidden_keys{
          "nodeRid", "node_rid", "actorRef", "actor_ref", "sessionRoute",
          "session_route", "sessionRouteKey", "session_route_key", "ownerNodeRid",
          "ownerActorRef"};
        std::function<void (const nlohmann::json &)> inspect;
        inspect = [&] (const nlohmann::json &value) {
            if (value.is_object ()) {
                for (const auto &[key, child] : value.items ()) {
                    if (std::find (forbidden_keys.begin (), forbidden_keys.end (), key)
                        != forbidden_keys.end ()) {
                        throw std::runtime_error (std::string (context)
                                                  + " exposes framework ownership field: " + key);
                    }
                    inspect (child);
                }
            } else if (value.is_array ()) {
                for (const auto &child : value) {
                    inspect (child);
                }
            }
        };
        inspect (wire);
    }

    static void expect (bool condition, const std::string &message)
    {
        if (!condition) {
            throw std::runtime_error (message);
        }
    }
};

} // namespace zlink::samples::supportchat
