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
#include "../support/stream_connector_failure.hpp"

using namespace stream_connector_fixture;


namespace stream_connector_tests
{
int verify_subscription_lifetime (zlink::stream_connector::connector_t &connector)
{
    /* stream-connector §7: every registration returns a handle that removes it,
   * removing twice is not an error, and a removed handler does not run. */
    {
        zlink::stream_connector::connector_t subscription_connector;
        auto subscription_runtime =
          zlink::stream_connector::detail::connector_runtime_t::from (subscription_connector);
        int kept_count = 0;
        int dropped_count = 0;
        auto kept = subscription_connector.on<zlink::stream_connector::packet_t> (
          "subscription.push", [&] (const packet_message_t &) { ++kept_count; });
        {
            auto scoped = subscription_connector.on<zlink::stream_connector::packet_t> (
              "subscription.push", [&] (const packet_message_t &) { ++dropped_count; });
            if (!scoped.active ()) {
                return fail (237);
            }
        }
        auto explicit_handle = subscription_connector.on<zlink::stream_connector::packet_t> (
          "subscription.push", [&] (const packet_message_t &) { ++dropped_count; });
        explicit_handle.unsubscribe ();
        explicit_handle.unsubscribe ();
        if (explicit_handle.active () || !kept.active ()) {
            return fail (238);
        }
        subscription_runtime.receive_packet (
          zlink::stream_connector::packet_t{"subscription.push",
                                            {},
                                            zlink::stream_connector::codec_t::raw,
                                            false,
                                            as_bytes (std::string ("payload"))});
        while (subscription_connector.pending_dispatch_count () != 0) {
            if (!subscription_connector.dispatch ()) {
                return fail (239);
            }
        }
        if (kept_count != 1 || dropped_count != 0) {
            return fail (240);
        }
        kept.unsubscribe ();
        subscription_runtime.receive_packet (
          zlink::stream_connector::packet_t{"subscription.push",
                                            {},
                                            zlink::stream_connector::codec_t::raw,
                                            false,
                                            as_bytes (std::string ("payload"))});
        while (subscription_connector.pending_dispatch_count () != 0) {
            if (!subscription_connector.dispatch ()) {
                return fail (241);
            }
        }
        if (kept_count != 1) {
            return fail (242);
        }
        subscription_connector.close ();
    }

    /* A state transition can queue delivery before a screen releases its
   * subscriptions. Dispatch resolves the live registry at execution. */
    {
        zlink::stream_connector::connector_t connector;
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (connector));
        int removed_state_calls = 0;
        int removed_disconnect_calls = 0;
        int retained_state_calls = 0;
        auto removed_state =
          connector.on_connection_state_changed ([&] (const auto &) { ++removed_state_calls; });
        auto removed_disconnect =
          connector.on_disconnected ([&] (const auto &) { ++removed_disconnect_calls; });
        auto retained_state =
          connector.on_connection_state_changed ([&] (const auto &) { ++retained_state_calls; });
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::disconnected);
        removed_state.unsubscribe ();
        removed_disconnect.unsubscribe ();
        if (!connector.dispatch () || removed_state_calls != 0 || removed_disconnect_calls != 0
            || retained_state_calls != 1) {
            return fail (302);
        }
        retained_state.unsubscribe ();
        connector.close ();
    }
    {
        zlink::stream_connector::connector_t connector;
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (connector));
        int removed_error_calls = 0;
        auto removed_error = connector.on_error ([&] (const auto &) { ++removed_error_calls; });
        zlink::stream_connector::detail::publish_error (
          *state, {zlink::stream_connector::error_code_t::remote_error, "stream error"});
        removed_error.unsubscribe ();
        if (!connector.dispatch () || removed_error_calls != 0) {
            return fail (308);
        }
        connector.close ();
    }

    return 0;
}
} // namespace stream_connector_tests
