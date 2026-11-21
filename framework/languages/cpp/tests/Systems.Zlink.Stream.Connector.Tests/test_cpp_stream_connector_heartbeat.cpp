/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>
#include <zlink/stream_connector.hpp>
#include <zlink/stream_connector/codecs/auto_codec.hpp>
#include <zlink/stream_e2e_client.hpp>
#include <zlink/stream_e2e_client/codecs/auto_codec.hpp>
#include "runtime/connector_runtime.hpp"
#include "runtime/protocol/framing.hpp"
#include "runtime/protocol/framing/frame_codec.hpp"
#include "runtime/protocol/header_codec.hpp"
#include "runtime/protocol/metadata_codec.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <zlink/json.hpp>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>
#include "../support/stream_connector_cases.hpp"

#include "../support/stream_connector_packet_message.hpp"
#include "../support/stream_connector_wire_frames.hpp"
#include "../support/stream_connector_typed_messages.hpp"
#include "../support/stream_connector_callback_sync.hpp"
#include "../support/stream_connector_failure.hpp"

using namespace stream_connector_fixture;


namespace stream_connector_tests
{
int verify_heartbeat (
  zlink::context_t &context,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request)
{
    zlink::stream_socket_t heartbeat_server (context);
    heartbeat_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    heartbeat_server.options ().notify (false);
    heartbeat_server.bind ("tcp://127.0.0.1:0");
    const auto heartbeat_endpoint = heartbeat_server.options ().last_endpoint ();
    std::atomic_bool heartbeat_seen{false};
    joining_thread_t heartbeat_server_thread ([&heartbeat_server, &heartbeat_seen] {
        zlink::received_t inbound;
        if (heartbeat_server.recv (inbound) != 0) {
            return;
        }
        std::string buffer =
          inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
        if (auto frame = try_read_server_frame (buffer)) {
            heartbeat_seen = frame->header.kind == zlink::stream_connector::message_kind_t::control
                             && frame->header.name == "$zlink.heartbeat.ping";
            auto pong = make_server_frame (zlink::stream_connector::message_kind_t::control, 0,
                                           "$zlink.heartbeat.pong", "");
            inbound.send ().message (pong).submit ();
        }
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t heartbeat_options;
    heartbeat_options.endpoint = heartbeat_endpoint;
    /* stream-connector §6.3 rejects a non-positive heartbeat interval, so the
   * smallest positive interval stands in for "ping at once" here. */
    heartbeat_options.heartbeat.interval = std::chrono::milliseconds (1);
    auto heartbeat_connector =
      zlink::stream_connector::connector_factory_t::create (heartbeat_options);
    if (!heartbeat_connector.connect () || !heartbeat_connector.dispatch ()) {
        return fail (37);
    }
    heartbeat_server_thread.join ();
    if (!heartbeat_seen) {
        return fail (38);
    }
    bool heartbeat_control_delivered = false;
    auto heartbeat_control_subscription =
      heartbeat_connector.on<zlink::stream_connector::packet_t> (
        "$zlink.heartbeat.pong",
        [&] (const packet_message_t &) { heartbeat_control_delivered = true; });
    if (!heartbeat_connector.dispatch () || heartbeat_control_delivered
        || heartbeat_connector.pending_dispatch_count () != 0) {
        return fail (44);
    }
    heartbeat_connector.close ();

    /* graceful-drain-handoff §7.2: 동기 request가 응답을 기다리는 동안에도 server
   * ping에 pong으로 답해야 한다. pong을 dispatch() 경로에만 두면, 응답이
   * heartbeat timeout보다 오래 걸리는 정상 요청에서 서버가 세션을 끊는다(E2E
   * ATD-C3B). */
    zlink::stream_socket_t pong_during_request_server (context);
    pong_during_request_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    pong_during_request_server.options ().notify (false);
    pong_during_request_server.bind ("tcp://127.0.0.1:0");
    const auto pong_during_request_endpoint =
      pong_during_request_server.options ().last_endpoint ();
    std::atomic_bool pong_during_request_seen{false};
    joining_thread_t pong_during_request_thread ([&pong_during_request_server,
                                                  &pong_during_request_seen] {
        zlink::received_t inbound;
        if (pong_during_request_server.recv (inbound) != 0) {
            return;
        }
        std::string buffer =
          inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
        auto request = try_read_server_frame (buffer);
        if (!request || !request->header.request_seq) {
            inbound.close ();
            return;
        }
        /* 요청을 받아 두고 응답을 미룬 채 ping을 보낸다. */
        auto ping = make_server_frame (zlink::stream_connector::message_kind_t::control, 0,
                                       "$zlink.heartbeat.ping", "");
        inbound.send ().message (ping).submit ();

        /* 클라이언트가 dispatch()를 부르지 않는 동안 pong이 오는지 본다. */
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (3000);
        std::string pong_buffer;
        while (std::chrono::steady_clock::now () < deadline && !pong_during_request_seen) {
            zlink::received_t pong_inbound;
            if (pong_during_request_server.recv (pong_inbound) != 0) {
                break;
            }
            pong_buffer += pong_inbound.parts ().empty () ? std::string{}
                                                          : pong_inbound.parts ()[0].to_string ();
            if (auto frame = try_read_server_frame (pong_buffer)) {
                pong_during_request_seen =
                  frame->header.kind == zlink::stream_connector::message_kind_t::control
                  && frame->header.name == "$zlink.heartbeat.pong";
            }
        }

        auto reply = make_server_frame (zlink::stream_connector::message_kind_t::response,
                                        *request->header.request_seq, request->header.name, "{}");
        inbound.send ().message (reply).submit ();
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t pong_during_request_options;
    pong_during_request_options.endpoint = pong_during_request_endpoint;
    pong_during_request_options.heartbeat.interval = std::chrono::milliseconds (1);
    /* manual dispatch: application이 dispatch()를 부를 때까지 pump가 돌지 않는다.
   * 동기 request가 응답을 기다리는 동안 pong을 dispatch() 경로에만 두면 이
   * 모드에서 답이 나가지 않는다. */
    pong_during_request_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    auto pong_during_request_connector =
      zlink::stream_connector::connector_factory_t::create (pong_during_request_options);
    if (!pong_during_request_connector.connect ()) {
        return fail (190);
    }
    auto pong_during_request_reply = pong_during_request_connector.request (login_request_t{})
                                       .packet_name ("slow.request")
                                       .timeout (std::chrono::milliseconds (5000))
                                       .submit<login_reply_t> ();
    pong_during_request_thread.join ();
    pong_during_request_connector.close ();
    if (!pong_during_request_seen) {
        return fail (191);
    }
    if (!pong_during_request_reply) {
        return fail (192);
    }

    zlink::stream_socket_t heartbeat_timeout_server (context);
    heartbeat_timeout_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    heartbeat_timeout_server.options ().notify (false);
    heartbeat_timeout_server.bind ("tcp://127.0.0.1:0");
    zlink::stream_connector::connector_options_t heartbeat_timeout_options;
    heartbeat_timeout_options.endpoint = heartbeat_timeout_server.options ().last_endpoint ();
    heartbeat_timeout_options.heartbeat.timeout = std::chrono::milliseconds (1);
    // Keep the reconnecting state observable long enough to assert the
    // timeout transition instead of racing an immediate reconnect.
    heartbeat_timeout_options.reconnect.initial_delay = std::chrono::milliseconds (100);
    auto heartbeat_timeout_connector =
      zlink::stream_connector::connector_factory_t::create (heartbeat_timeout_options);
    if (!heartbeat_timeout_connector.connect ()) {
        return fail (45);
    }
    const auto heartbeat_timeout_deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (250);
    while (heartbeat_timeout_connector.state ()
             != zlink::stream_connector::connection_state_t::reconnecting
           && std::chrono::steady_clock::now () < heartbeat_timeout_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    if (heartbeat_timeout_connector.state ()
        != zlink::stream_connector::connection_state_t::reconnecting) {
        return fail (46);
    }
    heartbeat_timeout_connector.close ();

    return verify_websocket_and_tls_transport (context, connector, runtime);
}
} // namespace stream_connector_tests
