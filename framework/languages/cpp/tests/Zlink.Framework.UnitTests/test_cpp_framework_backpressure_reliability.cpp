/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

#include "runtime/channels/channel_runtime_bundle.hpp"
#include "runtime/channels/channel_runtime.hpp"

int main ()
{
    zlink::framework::zlink_builder_t zlink;
    zlink.max_pending (1);
    zlink.channel ("profile").enable_client ().connect ("tcp://127.0.0.1:7400");

    auto bus = zlink.message_bus ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (bus);
    if (runtime.pending_limit () != 1) {
        return 1;
    }

    auto first_request = runtime.reserve_outbound_request ("profile");
    if (!first_request || runtime.pending_count () != 1) {
        return 2;
    }

    auto full_result = runtime.reserve_outbound_request ("profile");
    if (full_result
        || full_result.error_kind ()
             != zlink::framework::framework_error_kind_t::rejected) {
        return 3;
    }

    auto completed = runtime.complete_outbound_reply (first_request.value ());
    if (!completed || runtime.pending_count () != 0) {
        return 4;
    }

    auto drained_request = runtime.reserve_outbound_request ("profile");
    if (!drained_request) {
        return 5;
    }
    runtime.drain ();
    if (runtime.pending_count () != 0) {
        return 6;
    }

    zlink::framework::zlink_builder_t shutdown_builder;
    shutdown_builder.channel ("profile").enable_client ().connect ("tcp://127.0.0.1:7401");
    auto shutdown_runtime =
      zlink::framework::detail::channel_runtime_t::from (shutdown_builder.message_bus ());
    auto shutdown_pending = shutdown_runtime.reserve_outbound_request ("profile");
    if (!shutdown_pending) {
        return 7;
    }
    shutdown_runtime.shutdown ();
    if (shutdown_runtime.pending_count () != 0) {
        return 8;
    }
    auto after_shutdown = shutdown_runtime.reserve_outbound_request ("profile");
    if (after_shutdown
        || (after_shutdown.error () != nullptr
         && zlink::framework::detail::boundary_state (*after_shutdown.error ()) != zlink::framework::detail::boundary_error_t::shutdown)) {
        return 9;
    }

    zlink::framework::zlink_builder_t close_builder;
    close_builder.channel ("profile").enable_client ().connect ("tcp://127.0.0.1:7402");
    auto close_runtime =
      zlink::framework::detail::channel_runtime_t::from (close_builder.message_bus ());
    close_runtime.close ();
    auto after_close = close_runtime.reserve_outbound_request ("profile");
    if (after_close
        || (after_close.error () != nullptr
         && zlink::framework::detail::boundary_state (*after_close.error ()) != zlink::framework::detail::boundary_error_t::closed)) {
        return 10;
    }

    zlink::framework::detail::channel_runtime_bundle_t subscriber_bundle;
    if (!subscriber_bundle.try_add_manual_connection ("tcp://subscriber-a:7400")
        || !subscriber_bundle.try_add_manual_connection ("tcp://subscriber-b:7400")
        || !subscriber_bundle.try_add_manual_connection ("tcp://subscriber-c:7400")) {
        return 11;
    }
    if (!subscriber_bundle.try_enter_receive ()) {
        return 12;
    }
    if (subscriber_bundle.try_enter_receive ()) {
        return 13;
    }
    subscriber_bundle.remove_manual_connection ("tcp://subscriber-b:7400");
    const auto remaining_subscribers = subscriber_bundle.list_manual_connections ();
    if (remaining_subscribers.size () != 2 || remaining_subscribers[0] != "tcp://subscriber-a:7400"
        || remaining_subscribers[1] != "tcp://subscriber-c:7400") {
        return 14;
    }
    subscriber_bundle.leave_receive ();
    if (subscriber_bundle.receive_active () || !subscriber_bundle.try_enter_receive ()) {
        return 15;
    }
    subscriber_bundle.leave_receive ();

    return 0;
}
