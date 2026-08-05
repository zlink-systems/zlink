/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Shared/Contracts/messages.hpp"

#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <zlink/stream_connector.hpp>
#include <zlink/stream_e2e_client.hpp>
#include <zlink/stream_e2e_client/codecs/auto_codec.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
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

        std::cout << "supportchat authentication=verified" << std::endl;
        std::cout << "supportchat conversation-assignment=verified" << std::endl;
        std::cout << "supportchat bound-push=verified" << std::endl;
        std::cout << "supportchat reconnect=verified" << std::endl;
        std::cout << "supportchat idle-close=verified" << std::endl;
        std::cout << "supportchat=completed" << std::endl;
    }

  private:
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

        auto customer_availability =
          customer.request (set_agent_available_req_t{true})
            .async<set_agent_available_res_t> ()
            .result ();
        expect (!customer_availability, "customer must not set agent availability");

        auto available = request<set_agent_available_res_t> (
          agent, set_agent_available_req_t{true}, "agent availability failed");
        expect (available.is_available, "agent was not made available");

        auto assigned = wait_assigned_packet (agent);
        auto joined = wait_joined (customer, "participant join wait failed");
        auto agent_message = wait_chat (agent, "Payment keeps failing.");
        auto customer_typing = wait_typing (customer, "agent-1", true);
        auto customer_message = wait_chat (customer, "Please retry your card.");

        auto opened = request<open_conversation_res_t> (
          customer, open_conversation_req_t{"checkout payment failed"}, "open conversation failed");
        expect (opened.state.status == conversation_status_t::waiting_for_agent,
                "new conversation must remain waiting until the agent joins");
        expect (assigned.get ().conversation_id == opened.conversation_id,
                "assignment notification mismatch");

        auto agent_joined = request_in_conversation<join_conversation_res_t> (
          agent, opened.conversation_id, join_conversation_req_t{}, "agent join failed");
        expect (agent_joined.scheduled,
                "agent join must report a deferred membership operation");
        expect (agent_joined.state.status == conversation_status_t::waiting_for_agent,
                "scheduled agent join must return the pre-commit state");
        expect (joined.get ().actor_id == "agent-1", "participant join notification mismatch");

        auto sent = request_in_conversation<send_chat_message_res_t> (
          customer, opened.conversation_id, send_chat_message_req_t{"Payment keeps failing."},
          "customer message failed");
        expect (sent.message.message_seq == 1, "first message sequence mismatch");
        /* sentAtUnixMs is a wall-clock value and the system clock may be adjusted while
         * a request is in flight. MessageSeq, rather than wall-clock ordering, defines
         * message order; the wire timestamp must still be populated. */
        expect (sent.message.sent_at_unix_ms > 0,
                "message timestamp must contain a positive Unix time");
        expect (agent_message.get ().message.text == "Payment keeps failing.",
                "agent did not receive customer message");

        agent.send (set_typing_msg_t{true})
          .metadata (conversation_id_metadata_key, opened.conversation_id)
          .submit ();
        expect (customer_typing.get ().is_typing, "customer did not receive typing notification");

        auto reply = request_in_conversation<send_chat_message_res_t> (
          agent, opened.conversation_id, send_chat_message_req_t{"Please retry your card."},
          "agent message failed");
        expect (reply.message.message_seq == 2, "second message sequence mismatch");
        expect (customer_message.get ().message.text == "Please retry your card.",
                "customer did not receive agent message");

        /* 같은 상담원이 용량 안에서 두 번째 방을 받는다(공통 sample spec §17-13~17). */
        auto second_customer_core = make_connector (endpoint);
        auto second_customer = zlink::stream_e2e_client::use (second_customer_core);
        expect (static_cast<bool> (second_customer.connect ().submit ()),
                "second customer stream connect failed");
        auto second_auth = request<authenticate_res_t> (
          second_customer, authenticate_req_t{"customer-2"}, "second customer auth failed");
        expect (second_auth.role == role_t::customer, "second customer role mismatch");

        auto second_assigned = wait_assigned_packet (agent);
        auto second_joined = wait_joined (second_customer, "second participant join wait failed");
        auto second_message = wait_chat (second_customer, "Checking your refund now.");

        auto second_opened =
          request<open_conversation_res_t> (second_customer,
                                            open_conversation_req_t{"refund not received"},
                                            "second open conversation failed");
        expect (second_opened.conversation_id != opened.conversation_id,
                "second conversation must have its own id");
        expect (second_assigned.get ().conversation_id == second_opened.conversation_id,
                "second assignment notification mismatch");

        auto second_agent_joined = request_in_conversation<join_conversation_res_t> (
          agent, second_opened.conversation_id, join_conversation_req_t{},
          "agent join of second conversation failed");
        expect (second_agent_joined.scheduled,
                "second agent join must report a deferred membership operation");
        expect (second_agent_joined.state.status == conversation_status_t::waiting_for_agent,
                "second scheduled join must return the pre-commit state");
        expect (second_joined.get ().actor_id == "agent-1",
                "second participant join notification mismatch");

        auto second_sent = request_in_conversation<send_chat_message_res_t> (
          agent, second_opened.conversation_id,
          send_chat_message_req_t{"Checking your refund now."}, "second conversation send failed");
        expect (second_sent.message.message_seq == 1,
                "second conversation sequence must start at 1");
        expect (second_message.get ().message.text == "Checking your refund now.",
                "second customer did not receive agent message");

        /* 방마다 MessageSeq와 push가 독립인지 확인한다(§17-17). */
        auto first_state = request_in_conversation<join_conversation_res_t> (
          agent, opened.conversation_id, join_conversation_req_t{}, "first conversation re-join failed");
        expect (!first_state.scheduled,
                "an already joined conversation must return current state");
        expect (first_state.state.last_message_seq == 2,
                "first conversation sequence must be independent of the second");

        /* conversation Spot의 idle 판정은 마지막 메시지 기준이다(§14). reconnect 검증이
         * idle 전이와 겹치지 않도록 첫 방에 메시지를 하나 더 보내 창을 다시 연다. */
        auto agent_keepalive = wait_chat (agent, "Still looking into it.");
        auto keepalive = request_in_conversation<send_chat_message_res_t> (
          customer, opened.conversation_id, send_chat_message_req_t{"Still looking into it."},
          "first conversation keep-alive failed");
        expect (keepalive.message.message_seq == 3, "keep-alive sequence mismatch");
        expect (agent_keepalive.get ().message.message_seq == 3,
                "agent did not receive the keep-alive message");

        /* reconnect: 같은 token으로 새 stream을 열고 열려 있던 방에 다시 join한다(§15, §17-19~20). */
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

        /* immediate dispatch mode does not retain an unmatched push packet. Register the
         * reconnect observations before the join requests so the timer-owned idle/closed
         * notifications cannot arrive between rejoin completion and wait registration. */
        auto agent_idle = wait_idle (reconnected_agent, opened.conversation_id,
                                     "reconnected agent idle notification wait failed");
        auto customer_idle = wait_idle (customer, opened.conversation_id,
                                        "customer idle notification wait failed");
        auto agent_closed = wait_closed (reconnected_agent, opened.conversation_id,
                                         "reconnected agent closed notification wait failed");
        auto customer_closed = wait_closed (customer, opened.conversation_id,
                                            "customer closed notification wait failed");

        auto rejoined_first = request_in_conversation<join_conversation_res_t> (
          reconnected_agent, opened.conversation_id, join_conversation_req_t{},
          "reconnected agent could not re-join the first conversation");
        expect (!rejoined_first.scheduled,
                "reconnect must not schedule an already committed Join");
        expect (rejoined_first.state.status == conversation_status_t::active,
                "first conversation state must survive the reconnect");
        expect (rejoined_first.state.last_message_seq == 3,
                "first conversation history must survive the reconnect");
        auto rejoined_second = request_in_conversation<join_conversation_res_t> (
          reconnected_agent, second_opened.conversation_id, join_conversation_req_t{},
          "reconnected agent could not re-join the second conversation");
        expect (!rejoined_second.scheduled,
                "second reconnect must return current state");
        expect (rejoined_second.state.last_message_seq == 1,
                "second conversation history must survive the reconnect");

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

        /* 유휴 감지와 종료는 conversation Spot timer가 소유한다(공통 sample spec §14):
         * 클라이언트는 아무것도 보내지 않고 idle -> closed 알림을 양쪽에서 기다린다(§17-21). */
        expect (agent_idle.get ().state.status == conversation_status_t::waiting_for_close,
                "agent did not receive idle notification");
        expect (customer_idle.get ().state.status == conversation_status_t::waiting_for_close,
                "customer did not receive idle notification");
        expect (agent_closed.get ().state.status == conversation_status_t::closed,
                "agent did not receive closed notification");
        expect (customer_closed.get ().state.status == conversation_status_t::closed,
                "customer did not receive closed notification");
    }

    static std::future<conversation_idle_notify_t> wait_idle (connector_t &connector,
                                                              std::string conversation_id,
                                                              const char *failure_message)
    {
        return connector.wait_for<conversation_idle_notify_t> ()
          .where (&conversation_idle_notify_t::conversation_id, std::move (conversation_id))
          .timeout (std::chrono::seconds (12))
          .to_future (failure_message);
    }

    static std::future<conversation_closed_notify_t> wait_closed (connector_t &connector,
                                                                  std::string conversation_id,
                                                                  const char *failure_message)
    {
        return connector.wait_for<conversation_closed_notify_t> ()
          .where (&conversation_closed_notify_t::conversation_id, std::move (conversation_id))
          .timeout (std::chrono::seconds (12))
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

    static std::future<participant_joined_notify_t> wait_joined (connector_t &connector,
                                                                 const char *failure_message)
    {
        return std::async (std::launch::async, [&connector, failure_message] {
            return connector.wait_for<participant_joined_notify_t> ()
              .timeout (std::chrono::seconds (12))
              .to_future (failure_message)
              .get ();
        });
    }

    static std::future<chat_message_notify_t> wait_chat (connector_t &connector,
                                                         const std::string &text)
    {
        return std::async (std::launch::async, [&connector, text] {
            auto message = connector.wait_for<chat_message_notify_t> ()
                             .timeout (std::chrono::seconds (12))
                             .to_future ("chat message wait failed")
                             .get ();
            if (message.message.text != text) {
                throw std::runtime_error ("chat message payload mismatch");
            }
            return message;
        });
    }

    static std::future<typing_changed_notify_t>
    wait_typing (connector_t &connector, const std::string &actor_id, bool is_typing)
    {
        return std::async (std::launch::async, [&connector, actor_id, is_typing] {
            auto message = connector.wait_for<typing_changed_notify_t> ()
                             .timeout (std::chrono::seconds (12))
                             .to_future ("typing wait failed")
                             .get ();
            if (message.actor_id != actor_id || message.is_typing != is_typing) {
                throw std::runtime_error ("typing payload mismatch");
            }
            return message;
        });
    }

    static void require_evidence (const std::vector<std::string> &evidence,
                                  const std::string &expected)
    {
        const auto found = std::find (evidence.begin (), evidence.end (), expected);
        expect (found != evidence.end (), "missing support evidence: " + expected);
    }

    static void expect (bool condition, const std::string &message)
    {
        if (!condition) {
            throw std::runtime_error (message);
        }
    }
};

} // namespace zlink::samples::supportchat
