/* SPDX-License-Identifier: Elastic-2.0 */
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/stateful/public_host_runtime.hpp"

#include <zlink/framework.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <tuple>
#include <utility>

namespace
{
namespace host = zlink::framework::runtime::host;
using namespace zlink::framework;
using namespace zlink::framework::detail;

int reply_submission_precedes_close ()
{
    unsigned failures = 0;
    for (unsigned iteration = 0; iteration != 40; ++iteration) {
        const auto owner_rid = zlink::routing_id_t::from ("ordered-session-owner");
        const auto actor_rid = zlink::routing_id_t::from ("ordered-actor-owner");
        const auto client_rid = zlink::routing_id_t::from ("ordered-client");
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
        unsigned closes = 0;
        const auto connection =
          owner->sessions ().open (client_rid.to_hex (), {}, owner_rid, 11, [&] () -> task_t<void> {
              ++closes;
              co_return;
          });
        const auto [bind_error, binding] =
          owner->sessions ().bind_remote (connection, actor, 11, 19);
        if (bind_error != runtime::stateful::stateful_error_t::none)
            return 1;
        const runtime::protocol::retired_bound_session_route_fence_t fence{
          owner_rid.to_bytes (),     11, "session-owner", 23, client_rid.to_bytes (),
          binding.binding_generation};
        const runtime::protocol::bound_session_close_t close{0, actor.key, actor.object_generation,
                                                             fence};
        auto [admission, dispatch] = owner->sessions ().admit_inbound (binding);
        if (admission != runtime::stateful::stateful_error_t::none || !dispatch)
            return 2;

        // This active ingress stands for the reply that the Session handler
        // has received from the bound Actor but has not yet submitted to STREAM.
        auto pending_close = owner->sessions ().close_current (close, fence);
        const bool overtaken = closes != 0 || pending_close.await_ready ();

        (void) owner->sessions ().complete_inbound (*dispatch);
        const auto terminal = pending_close.result ();
        if (overtaken || !terminal || closes != 1)
            ++failures;
        owner->close ();
    }
    std::cerr << "reply submission -> close: " << failures << "/40 ordering failures\n";
    return failures == 0 ? 0 : 1;
}

int push_admission_precedes_close ()
{
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
        if (!gateway.record_bound_session_route (actor, zlink::routing_id_t::from ("session-owner"),
                                                 zlink::routing_id_t::from ("client"), 11, 13, 17,
                                                 19, 0, 0))
            return 2;

        gateway.on_bound_session_close (
          [closes] (actor_ref_t, actor_bound_session_route_t, auto... submitter) -> task_t<void> {
              if constexpr (sizeof...(submitter) == 0) {
                  ++*closes;
                  co_return;
              } else {
                  auto submit = std::get<0> (std::tuple{std::move (submitter)...});
                  auto completion = std::make_shared<task_completion_source_t<void>> ();
                  auto task = completion->task ();
                  const bool queued = submit (
                    [closes] () -> task_t<bool> {
                        ++*closes;
                        co_return true;
                    },
                    [completion] (result_t<void> terminal) {
                        completion->complete (std::move (terminal));
                    });
                  if (!queued)
                      completion->complete (result_t<void>::failure (
                        framework_error_kind_t::unavailable, "close was not queued"));
                  co_return co_await task;
              }
          });

        auto session = gateway.actor_context (actor).bound_session ();
        const auto sent = session.send (std::string ("push")).async ().result ();
        if (!sent || started.wait_for (std::chrono::seconds (2)) != std::future_status::ready) {
            admission->complete (result_t<void>::success ());
            return 3;
        }
        auto pending_close = session.disconnect ();
        const bool overtaken = closes->load () != 0 || pending_close.await_ready ();
        admission->complete (result_t<void>::success ());
        const auto terminal = pending_close.result ();
        if (overtaken || !terminal || closes->load () != 1)
            ++failures;
    }
    std::cerr << "push admission -> close: " << failures << "/40 ordering failures\n";
    return failures == 0 ? 0 : 1;
}
} // namespace

int main (int argc, char **argv)
{
    completion_owner_t completion_owner;
    runtime::install_host_context_hooks ();
    if (argc == 2) {
        const std::string test (argv[1]);
        if (test == "--reply-close-order")
            return reply_submission_precedes_close ();
        if (test == "--push-close-order")
            return push_admission_precedes_close ();
        return 2;
    }
    const auto reply = reply_submission_precedes_close ();
    const auto push = push_admission_precedes_close ();
    return reply ? reply : push;
}
