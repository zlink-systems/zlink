/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/stream_session_registry.hpp"
#include <zlink/framework.hpp>
#include <zlink.hpp>
#include "runtime/streams/stream_runtime.hpp"
#include <boost/asio.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{
namespace host = zlink::framework::runtime::host;
int close_follows_session_delivery_submission ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    unsigned failures = 0;
    for (unsigned iteration = 0; iteration != 40; ++iteration) {
        const auto owner_rid = zlink::routing_id_t::from ("ordered-session-owner");
        const auto actor_rid = zlink::routing_id_t::from ("ordered-actor-owner");
        const auto client_rid = zlink::routing_id_t::from ("ordered-client");
        const auto owner = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
          .mesh = {.descriptor = {.mesh_name = "ordered-close",
                                  .node_routing_id = owner_rid.to_bytes (),
                                  .lifecycle_generation = 11,
                                  .descriptor_revision = 1,
                                  .advertised_endpoint = "tcp://127.0.0.1:0"},
                   .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
        const runtime::stateful::object_ref_t actor{runtime::stateful::object_kind_t::actor,
                                                    "ordered-actor",
                                                    7,
                                                    13,
                                                    "ordered-close",
                                                    actor_rid.to_string ()};
        unsigned closes = 0;
        unsigned submissions = 0;
        bool overtaken = false;
        auto &sessions = owner->sessions ();
        const auto connection =
          sessions.open (client_rid.to_hex (), {}, owner_rid, 11, [&] () -> task_t<void> {
              overtaken = overtaken || submissions != 1;
              ++closes;
              co_return;
          });
        const auto [error, binding] = sessions.bind_remote (connection, actor, 17, 19);
        if (error != runtime::stateful::stateful_error_t::none)
            return 1;
        const runtime::protocol::retired_bound_session_route_fence_t fence{
          owner_rid.to_bytes (),     11, "session-owner", 23, client_rid.to_bytes (),
          binding.binding_generation};
        const runtime::protocol::bound_session_close_t close{0, actor.key, actor.object_generation,
                                                             fence};
        auto stale = close;
        ++stale.session.retired_binding_generation;
        const auto rejected = sessions.close_current (stale, fence).result ();
        if (rejected || rejected.error_kind () != framework_error_kind_t::unavailable
            || closes != 0)
            return 4;
        std::optional<task_t<void>> pending_close;
        host::bound_session_operations_t operations;
        operations.bind = [] (const auto &, const auto &, auto, const auto &, auto) {
            return task_t<host::bound_session_bind_operation_result_t> (
              result_t<host::bound_session_bind_operation_result_t>::success ({}));
        };
        operations.send = [] (const auto &, auto) {
            return runtime::stateful::stateful_error_t::conflict;
        };
        operations.replaced = [] (const auto &) {};
        operations.commit_relocation_route = [] (const auto &, const auto &, const auto &) {
            return false;
        };
        operations.prepare_relocation_target_route = [] (const auto &, auto) { return false; };
        operations.capture_send = [&] (const runtime::protocol::bound_session_send_t &)
          -> std::optional<host::bound_session_operations_t::delivery_capability_t> {
            return [&] (std::vector<zlink::message_t>) {
                // Register close while delivery has begun but Core submission has not.
                std::thread register_close (
                  [&] { pending_close.emplace (sessions.close_current (close, fence)); });
                register_close.join ();
                overtaken = overtaken || closes != 0;
                ++submissions;
                return task_t<runtime::stateful::stateful_error_t> (
                  result_t<runtime::stateful::stateful_error_t>::success (
                    runtime::stateful::stateful_error_t::none));
            };
        };
        owner->configure_bound_session_operations (std::move (operations));
        const runtime::protocol::bound_session_send_t send{{actor.key, actor.object_generation,
                                                            actor_rid.to_bytes (), 17,
                                                            actor.authority_owner_generation, 19},
                                                           binding.binding_generation};
        const runtime::protocol::application_payload_t application{
          std::string (runtime::protocol::framework_multipart_packet_name),
          std::string (runtime::protocol::framework_multipart_content_type),
          {0, 0, 0, 1, 0, 0, 0, 2, 0x7b, 0x7d}};
        if (!owner->transport ().mailbox ().try_enqueue (
              runtime::mesh::service_mailbox_record_t::from_parts (
                "bound-session:" + actor.key, runtime::mesh::service_mailbox_domain_t::application,
                backend::materialize_binding_parts (
                  {runtime::protocol::encode_bound_session_send (send),
                   runtime::protocol::encode_application_payload (application)}),
                actor_rid.to_bytes (), std::nullopt, 17)))
            return 2;
        const auto dispatched =
          owner
            ->dispatch_ready ([] (const host::ready_record_t &, const host::receive_record_t &,
                                  std::vector<zlink::message_t>, auto) {})
            .result ();
        if (!dispatched || !pending_close) {
            std::cerr << "Session delivery setup failed: "
                      << (dispatched ? "delivery was not invoked" : dispatched.error ()->what ())
                      << '\n';
            return 3;
        }
        const auto closed = pending_close->result ();
        if (!closed || overtaken || submissions != 1 || closes != 1) {
            if (failures == 0)
                std::cerr << "Session ordering failure: submissions=" << submissions
                          << " closes=" << closes << " overtaken=" << overtaken
                          << " terminal=" << (closed ? "ok" : closed.error ()->what ()) << '\n';
            ++failures;
        }
        owner->close ();
    }
    std::cerr << "Session delivery submission -> close: " << failures << "/40 ordering failures\n";
    return failures ? 1 : 0;
}

int close_waits_for_bound_session_push (bool local = false)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    unsigned failures = 0;
    for (unsigned iteration = 0; iteration != 40; ++iteration) {
        auto state = std::make_shared<actor_gateway_state_t> ();
        serializer_registry_t serializers;
        state->serializers = &serializers;
        actor_gateway_runtime_t gateway (state);
        const auto actor = actor_ref_access_t::make (node_rid_t::from_string ("local-node"),
                                                     "player", "ordered-close", 7);
        auto submitted = std::make_shared<std::promise<void>> ();
        auto started = submitted->get_future ();
        auto admission = std::make_shared<task_completion_source_t<void>> ();
        auto closes = std::make_shared<std::atomic_uint> (0);
        if (!gateway.bind_session_sink (actor, [submitted, admission] (std::string, stream_codec_t,
                                                                       const zlink::message_t &) {
                submitted->set_value ();
                return admission->task ();
            }))
            return 1;
        if (!gateway.record_bound_session_route (actor, zlink::routing_id_t::from ("local-node"),
                                                 zlink::routing_id_t::from ("client"), 11, 13, 17,
                                                 19, 0, 0))
            return 2;
        if (local)
            state->sync ([&] {
                state->actors_by_id.at (std::string (actor.actor_id ().value ()))
                  .bound_session_stream_sink = true;
            });
        gateway.on_bound_session_close (
          [closes] (actor_ref_t, actor_bound_session_route_t,
                    actor_gateway_state_t::bound_session_close_submit_t submit) {
              auto completion = std::make_shared<task_completion_source_t<void>> ();
              auto task = completion->task ();
              if (!submit (
                    [closes, completion] () -> task_t<bool> {
                        ++*closes;
                        completion->complete (result_t<void>::success ());
                        co_return true;
                    },
                    [completion] (result_t<void> terminal) {
                        if (!terminal)
                            completion->complete (std::move (terminal));
                    }))
                  completion->complete (result_t<void>::failure (
                    framework_error_kind_t::unavailable, "close was not queued"));
              return task;
          });
        auto session = gateway.actor_context (actor).bound_session ();
        auto sent = session.send (std::string ("push")).async ().result ();
        if (!sent || started.wait_for (std::chrono::seconds (2)) != std::future_status::ready) {
            admission->complete (result_t<void>::success ());
            return 3;
        }
        auto closed = session.disconnect ();
        const bool overtaken = closes->load () != 0 || closed.await_ready ();
        admission->complete (result_t<void>::success ());
        const auto terminal = closed.result ();
        if (overtaken || !terminal || closes->load () != 1)
            ++failures;
    }
    std::cerr << (local ? "local" : "detached") << " push admission -> close: " << failures
              << "/40 ordering failures\n";
    return failures ? 1 : 0;
}

int client_observes_push_before_eof (bool ordered = true)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace asio = boost::asio;
    unsigned failures = 0;
    for (unsigned iteration = 0; iteration != 40; ++iteration) {
        zlink::context_t context;
        auto socket = std::make_shared<zlink::stream_socket_t> (context);
        socket->options ().recv_timeout (std::chrono::seconds (2));
        socket->options ().recv_mode (zlink::stream_recv_mode_t::packet);
        socket->bind ("tcp://127.0.0.1:*");
        const auto endpoint = socket->options ().last_endpoint ();
        const auto port =
          static_cast<unsigned short> (std::stoul (endpoint.substr (endpoint.rfind (':') + 1)));
        asio::io_context io;
        asio::ip::tcp::socket client (io);
        client.connect (asio::ip::tcp::endpoint (asio::ip::address_v4::loopback (), port));
        stream_runtime_t codec (std::make_shared<stream_runtime_state_t> ());
        const stream_header_t header (stream_message_kind_t::send, stream_codec_t::raw,
                                      stream_header_flags_t::none, std::nullopt, "ordered-push");
        const auto payload = zlink::message_t::from ("payload-before-eof");
        const auto encoded = codec.encode_frame (header, payload).value ();
        asio::write (client, asio::buffer (encoded));
        zlink::stream_packet_t packet;
        if (!socket->recv_packet (packet) || !packet.routing_id ())
            return 1;
        const auto rid = *packet.routing_id ();
        auto state = std::make_shared<actor_gateway_state_t> ();
        serializer_registry_t serializers;
        state->serializers = &serializers;
        actor_gateway_runtime_t gateway (state);
        const auto actor = actor_ref_access_t::make (node_rid_t::from_string ("wire-node"),
                                                     "player", "wire-actor", 7);
        auto entered = std::make_shared<std::promise<void>> ();
        auto started = entered->get_future ();
        auto release = std::make_shared<task_completion_source_t<void>> ();
        auto delivered = std::make_shared<task_completion_source_t<void>> ();
        if (!gateway.bind_session_sink (
              actor,
              [socket, rid, encoded, entered, release,
               delivered] (std::string, stream_codec_t, const zlink::message_t &) -> task_t<void> {
                  entered->set_value ();
                  co_await release->task ();
                  try {
                      auto operation =
                        socket->send (rid).message (zlink::message_t::from (encoded)).async ();
                      co_await std::move (operation.admitted);
                      delivered->complete (result_t<void>::success ());
                  }
                  catch (...) {
                      delivered->complete (current_exception_result<void> ());
                      throw;
                  }
              }))
            return 2;
        if (!gateway.record_bound_session_route (actor, zlink::routing_id_t::from ("wire-node"),
                                                 rid, 11, 13, 17, 19, 0, 0))
            return 3;
        gateway.on_bound_session_close (
          [socket, rid, ordered] (actor_ref_t, actor_bound_session_route_t,
                                  actor_gateway_state_t::bound_session_close_submit_t submit) {
              if (!ordered) {
                  socket->disconnect_rid (rid);
                  return task_t<void> (result_t<void>::success ());
              }
              auto completion = std::make_shared<task_completion_source_t<void>> ();
              if (!submit (
                    [socket, rid, completion] () -> task_t<bool> {
                        try {
                            socket->disconnect_rid (rid);
                            completion->complete (result_t<void>::success ());
                        }
                        catch (...) {
                            completion->complete (current_exception_result<void> ());
                        }
                        co_return true;
                    },
                    [completion] (result_t<void> terminal) {
                        if (!terminal)
                            completion->complete (std::move (terminal));
                    }))
                  completion->complete (result_t<void>::failure (
                    framework_error_kind_t::unavailable, "close was not registered"));
              return completion->task ();
          });
        auto session = gateway.actor_context (actor).bound_session ();
        if (!session.send (std::string ("push")).async ().result ())
            return 4;
        if (started.wait_for (std::chrono::seconds (2)) != std::future_status::ready) {
            release->complete (result_t<void>::success ());
            return 5;
        }
        auto closed = session.disconnect ();
        release->complete (result_t<void>::success ());
        const auto close_terminal = closed.result ();
        const auto write_terminal = delivered->task ().result ();
        client.non_blocking (true);
        std::vector<std::uint8_t> received;
        boost::system::error_code error;
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
        while (std::chrono::steady_clock::now () < deadline) {
            std::array<std::uint8_t, 4096> bytes{};
            const auto count = client.read_some (asio::buffer (bytes), error);
            if (!error) {
                received.insert (received.end (), bytes.begin (), bytes.begin () + count);
            } else if (error == asio::error::would_block || error == asio::error::try_again) {
                std::this_thread::yield ();
            } else {
                break;
            }
        }
        if (!close_terminal || !write_terminal || error != asio::error::eof
            || received != encoded) {
            if (failures == 0)
                std::cerr << "client push failure: received=" << received.size ()
                          << " expected=" << encoded.size () << " terminal=" << error.message ()
                          << " close=" << static_cast<bool> (close_terminal)
                          << " write=" << static_cast<bool> (write_terminal) << '\n';
            ++failures;
        }
        socket->close ();
    }
    std::cerr << "client push payload -> EOF: " << failures << "/40 failures\n";
    return failures ? 1 : 0;
}

int client_observes_reply_before_eof ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace asio = boost::asio;
    configure_stream_dispatch_executor ();
    struct executor_guard_t
    {
        ~executor_guard_t () { shutdown_stream_dispatch_executor (); }
    } executor_guard;
    unsigned failures = 0;
    for (unsigned iteration = 0; iteration != 40; ++iteration) {
        zlink::context_t context;
        auto socket = std::make_shared<zlink::stream_socket_t> (context);
        socket->options ().recv_timeout (std::chrono::seconds (2));
        socket->options ().recv_mode (zlink::stream_recv_mode_t::packet);
        socket->bind ("tcp://127.0.0.1:*");
        const auto endpoint = socket->options ().last_endpoint ();
        const auto port =
          static_cast<unsigned short> (std::stoul (endpoint.substr (endpoint.rfind (':') + 1)));
        asio::io_context io;
        asio::ip::tcp::socket client (io);
        client.connect (asio::ip::tcp::endpoint (asio::ip::address_v4::loopback (), port));

        auto wire =
          std::make_shared<stream_runtime_t> (std::make_shared<stream_runtime_state_t> ());
        const stream_header_t request_header (stream_message_kind_t::request, stream_codec_t::json,
                                              stream_header_flags_t::has_request_seq, 1,
                                              "ordered-reply");
        serializer_registry_t serializers;
        auto builder = test::runtime_failure_builder ();
        builder.stream ("ordered-reply").bind ("tcp://127.0.0.1:0");
        bind_stream_serializers (builder, serializers);
        const auto raw_request =
          message_to_raw (message_t::from (std::string ("request")), serializers);
        const auto encoded_request = wire->encode_frame (request_header, raw_request).value ();
        asio::write (client, asio::buffer (encoded_request));
        zlink::stream_packet_t packet;
        if (!socket->recv_packet (packet) || !packet.routing_id ())
            return 1;
        const auto rid = *packet.routing_id ();

        const auto owner_rid = zlink::routing_id_t::from ("ordered-session-owner");
        const auto actor_rid = zlink::routing_id_t::from ("ordered-actor-owner");
        auto owner = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
          .mesh = {.descriptor = {.mesh_name = "ordered-close",
                                  .node_routing_id = owner_rid.to_bytes (),
                                  .lifecycle_generation = 11,
                                  .descriptor_revision = 1,
                                  .advertised_endpoint = "tcp://127.0.0.1:0"},
                   .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
        const runtime::stateful::object_ref_t actor{runtime::stateful::object_kind_t::actor,
                                                    "ordered-actor",
                                                    7,
                                                    13,
                                                    "ordered-close",
                                                    actor_rid.to_string ()};
        const auto connection = owner->sessions ().open (rid.to_hex (), {}, owner_rid, 11,
                                                         [socket, rid] () -> task_t<void> {
                                                             socket->disconnect_rid (rid);
                                                             co_return;
                                                         });
        const auto [bind_error, binding] =
          owner->sessions ().bind_remote (connection, actor, 11, 19);
        if (bind_error != runtime::stateful::stateful_error_t::none)
            return 2;
        const runtime::protocol::retired_bound_session_route_fence_t fence{
          owner_rid.to_bytes (),     11, "session-owner", 23, rid.to_bytes (),
          binding.binding_generation};
        const runtime::protocol::bound_session_close_t close{0, actor.key, actor.object_generation,
                                                             fence};
        auto [admission, dispatch] = owner->sessions ().admit_inbound (binding);
        if (admission != runtime::stateful::stateful_error_t::none || !dispatch)
            return 3;

        auto streams = stream_runtime_t::from (builder);
        auto stream = streams.open_session ("ordered-reply");
        streams.set_session_identity (stream, rid);
        auto handler_entered = std::make_shared<std::promise<void>> ();
        auto handler_entered_future = handler_entered->get_future ();
        auto resume_handler = std::make_shared<task_completion_source_t<void>> ();
        auto started = std::make_shared<std::promise<void>> ();
        auto started_future = started->get_future ();
        auto release = std::make_shared<task_completion_source_t<void>> ();
        auto expected = std::make_shared<std::vector<std::uint8_t>> ();
        streams.attach_transport_writer (
          stream,
          [socket, rid, wire, started, release, expected] (
            const stream_header_t &header, const zlink::message_t &payload) -> task_t<void> {
              const auto submitted_header = header;
              const auto submitted_payload = payload;
              started->set_value ();
              co_await release->task ();
              *expected = wire->encode_frame (submitted_header, submitted_payload).value ();
              auto operation =
                socket->send (rid).message (zlink::message_t::from (*expected)).async ();
              co_await std::move (operation.admitted);
          });
        auto dispatch_done = std::make_shared<task_completion_source_t<void>> ();
        class reply_session_t final : public packet_stream_session_t
        {
          public:
            reply_session_t (runtime::stateful::stream_session_registry_t &sessions,
                             runtime::stateful::stream_dispatch_t dispatch,
                             std::shared_ptr<std::promise<void>> handler_entered,
                             std::shared_ptr<task_completion_source_t<void>> resume_handler) :
                sessions (sessions),
                dispatch (std::move (dispatch)),
                handler_entered (std::move (handler_entered)),
                resume_handler (std::move (resume_handler))
            {
            }
            task_t<void> on_connected (stream_t &) override { co_return; }
            task_t<void> on_disconnected (stream_t &) override { co_return; }
            task_t<void> on_error (stream_t &, const stream_error_t &) override { co_return; }
            task_t<void> on_packet (stream_t &stream,
                                    const session_message_context_t &,
                                    const message_t &) override
            {
                handler_entered->set_value ();
                co_await resume_handler->task ();
                auto reply_submission = current_stream_reply_submission ();
                if (!reply_submission) {
                    (void) sessions.complete_inbound (dispatch);
                    throw framework_exception_t (framework_error_kind_t::protocol_error,
                                                 "STREAM reply submission token is missing");
                }
                auto terminal = std::make_shared<task_t<void>> (reply_submission->terminal ());
                detail::observe_task_completion (*terminal,
                                                 [this, terminal] (const result_t<void> &) {
                                                     (void) terminal;
                                                     (void) sessions.complete_inbound (dispatch);
                                                 });
                co_await stream.reply_packet (message_t::from (std::string ("reply"))).async ();
            }
            runtime::stateful::stream_session_registry_t &sessions;
            runtime::stateful::stream_dispatch_t dispatch;
            std::shared_ptr<std::promise<void>> handler_entered;
            std::shared_ptr<task_completion_source_t<void>> resume_handler;
        } session (owner->sessions (), *dispatch, handler_entered, resume_handler);
        const auto dispatched = streams.dispatch_packet_async (
          session, stream, request_header, raw_request,
          [dispatch_done] (const result_t<void> &result) { dispatch_done->complete (result); });
        if (!dispatched)
            return 4;
        if (handler_entered_future.wait_for (std::chrono::seconds (2))
            != std::future_status::ready) {
            resume_handler->complete (result_t<void>::success ());
            release->complete (result_t<void>::success ());
            (void) owner->sessions ().complete_inbound (*dispatch);
            return 5;
        }
        auto pending_close = owner->sessions ().close_current (close, fence);
        const bool overtaken = pending_close.await_ready ();
        resume_handler->complete (result_t<void>::success ());
        if (started_future.wait_for (std::chrono::seconds (2)) != std::future_status::ready) {
            release->complete (result_t<void>::success ());
            (void) owner->sessions ().complete_inbound (*dispatch);
            (void) dispatch_done->task ().result ();
            (void) pending_close.result ();
            return 6;
        }
        release->complete (result_t<void>::success ());
        const auto dispatch_terminal = dispatch_done->task ().result ();
        const auto close_terminal = pending_close.result ();
        client.non_blocking (true);
        std::vector<std::uint8_t> received;
        boost::system::error_code error;
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
        while (std::chrono::steady_clock::now () < deadline) {
            std::array<std::uint8_t, 4096> bytes{};
            const auto count = client.read_some (asio::buffer (bytes), error);
            if (!error) {
                received.insert (received.end (), bytes.begin (), bytes.begin () + count);
            } else if (error == asio::error::would_block || error == asio::error::try_again) {
                std::this_thread::yield ();
            } else {
                break;
            }
        }
        if (overtaken || !dispatch_terminal || !close_terminal || error != asio::error::eof
            || received != *expected) {
            if (failures == 0)
                std::cerr << "client reply failure: received=" << received.size ()
                          << " expected=" << expected->size () << " terminal=" << error.message ()
                          << " dispatch=" << static_cast<bool> (dispatch_terminal)
                          << " close=" << static_cast<bool> (close_terminal)
                          << " overtaken=" << overtaken << '\n';
            if (failures == 0 && dispatch_terminal.error ())
                std::cerr << "client reply dispatch error: " << dispatch_terminal.error ()->what ()
                          << '\n';
            ++failures;
        }
        owner->close ();
        socket->close ();
    }
    std::cerr << "client reply packet -> EOF: " << failures << "/40 failures\n";
    return failures ? 1 : 0;
}

struct sink_resource_t
{
    explicit sink_resource_t (std::shared_ptr<std::atomic_bool> value) : alive (std::move (value))
    {
    }
    std::shared_ptr<std::atomic_bool> alive;
    ~sink_resource_t () { alive->store (false); }
};

auto finish_captured_delivery (zlink::framework::task_t<void> admitted,
                               std::shared_ptr<std::atomic_bool> alive)
  -> zlink::framework::task_t<void>
{
    co_await admitted;
    if (!alive->load ())
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::unavailable,
          "captured sink resource was released before admission completed");
    co_return;
}

int captured_delivery_owns_pending_sink ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    actor_gateway_runtime_t gateway;
    const auto actor = actor_ref_access_t::make (node_rid_t::from_string ("lifetime-node"),
                                                 "player", "lifetime-actor", 7);
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    auto alive = std::make_shared<std::atomic_bool> (true);
    auto resource = std::make_shared<sink_resource_t> (alive);
    if (!gateway.bind_session_sink (actor, [resource, admitted, alive] (std::string, stream_codec_t,
                                                                        const zlink::message_t &) {
            return finish_captured_delivery (admitted->task (), alive);
        }))
        return 1;
    resource.reset ();
    auto delivery = gateway.admit_bound_session_delivery (actor, 0);
    if (!delivery)
        return 2;
    auto pending = (*delivery) ("push", stream_codec_t::raw, zlink::message_t::from ("payload"));
    gateway.unbind_session_stream ("lifetime-actor");
    delivery.reset ();
    admitted->complete (result_t<void>::success ());
    const auto terminal = pending.result ();
    std::cerr << "captured sink lifetime: " << (terminal ? "pass" : terminal.error ()->what ())
              << '\n';
    return terminal ? 0 : 1;
}

int queued_close_keeps_original_deadline ()
{
    using namespace zlink::framework;
    const auto rid = zlink::routing_id_t::from ("deadline-owner");
    runtime::mesh::raw_mesh_node_owner_t node (runtime::mesh::raw_mesh_node_options_t{
      .descriptor = {.mesh_name = "ordered-close",
                     .node_routing_id = rid.to_bytes (),
                     .lifecycle_generation = 11,
                     .descriptor_revision = 1,
                     .advertised_endpoint = "tcp://127.0.0.1:0"},
      .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()});
    node.start ();
    const runtime::protocol::bound_session_close_t request{
      0,
      "deadline-actor",
      7,
      {rid.to_bytes (), 11, "owner", 13, zlink::routing_id_t::from ("client").to_bytes (), 17}};
    auto rejected = node.request_bound_session_close (
      request, std::chrono::seconds (30),
      [] (std::function<task_t<bool> ()>, std::function<void (result_t<void>)> complete) {
          complete (result_t<void>::failure (framework_error_kind_t::shutting_down,
                                             "controlled egress executor refusal"));
          return true;
      });
    const auto failed = rejected.result ();
    if (failed || failed.error_kind () != framework_error_kind_t::unavailable) {
        node.close ();
        return 2;
    }
    std::cerr << "queued close dispatch failure: pass\n";
    std::optional<std::function<task_t<bool> ()>> queued;
    auto close = node.request_bound_session_close (
      request, std::chrono::seconds (30),
      [&] (std::function<task_t<bool> ()> work, std::function<void (result_t<void>)>) {
          queued.emplace (std::move (work));
          return true;
      });
    if (!queued || close.await_ready ()) {
        std::cerr << "Close registration failed: queued=" << queued.has_value ()
                  << " ready=" << close.await_ready ();
        if (close.await_ready ()) {
            const auto terminal = close.result ();
            std::cerr << " terminal=" << (terminal ? "ok" : terminal.error ()->what ());
        }
        std::cerr << '\n';
        node.close ();
        return 1;
    }
    const auto expired =
      node.expire_requests (std::chrono::steady_clock::now () + std::chrono::minutes (1)).result ();
    const auto terminal = close.result ();
    const auto drained = (*queued) ().result ();
    const auto late = node.mailbox ().try_claim (
      runtime::mesh::service_mailbox_domain_t::infrastructure, 16, 65536);
    const bool passed = expired && expired.value () == 1 && !terminal
                        && terminal.error_kind () == framework_error_kind_t::deadline_exceeded
                        && drained && drained.value () && !late;
    node.close ();
    std::cerr << "queued close original deadline: " << (passed ? "pass" : "fail") << '\n';
    return passed ? 0 : 1;
}

} // namespace

int main (int argc, char **argv)
{
    zlink::framework::completion_owner_t completion_owner;
    zlink::framework::runtime::install_host_context_hooks ();
    if (argc == 2) {
        const std::string test (argv[1]);
        if (test == "--client-push-eof-negative")
            return client_observes_push_before_eof (false);
        if (test == "--client-push-eof")
            return client_observes_push_before_eof ();
        if (test == "--client-reply-eof")
            return client_observes_reply_before_eof ();
        if (test == "--close-push-order")
            return close_waits_for_bound_session_push ();
        if (test == "--close-local-push-order")
            return close_waits_for_bound_session_push (true);
        if (test == "--close-session-order")
            return close_follows_session_delivery_submission ();
        if (test == "--close-deadline")
            return queued_close_keeps_original_deadline ();
        if (test == "--delivery-lifetime")
            return captured_delivery_owns_pending_sink ();
        return 2;
    }
    const auto client = client_observes_push_before_eof ();
    if (client)
        return client;
    const auto reply = client_observes_reply_before_eof ();
    if (reply)
        return reply;
    const auto local = close_waits_for_bound_session_push (true);
    if (local)
        return local;
    const auto push = close_waits_for_bound_session_push ();
    const auto session = close_follows_session_delivery_submission ();
    const auto deadline = queued_close_keeps_original_deadline ();
    const auto lifetime = captured_delivery_owns_pending_sink ();
    return push ? push : session ? session : deadline ? deadline : lifetime;
}
