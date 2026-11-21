/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "channel_messaging_messages.hpp"
#include <zlink/framework/contracts/handlers/handler_registry.hpp>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace framework_channel_messaging_fixture
{
struct local_handler_state_t
{
    int last_request = 0;
    int last_event = 0;
    int last_route_request = 0;
    int last_route_event = 0;
    int internal_dispatch_provider_seen = 0;
    std::string last_route_source;
    std::thread::id last_route_request_thread;
    std::thread::id last_route_send_thread;
    bool *application_permit_released = nullptr;
    bool route_request_saw_permit_release = false;
    bool route_send_saw_permit_release = false;
    std::mutex send_gate_mutex;
    std::condition_variable send_gate_changed;
    bool blocking_send_entered = false;
    bool release_blocking_send = false;
};

class local_handler_t
{
  public:
    explicit local_handler_t (local_handler_state_t &state) : state (state) {}

    reply_t handle_request (const request_t &request)
    {
        state.last_request = request.value;
        return {request.value + 100};
    }

    void handle_send (const event_t &event)
    {
        if (event.value == 300) {
            std::unique_lock lock (state.send_gate_mutex);
            state.blocking_send_entered = true;
            state.send_gate_changed.notify_all ();
            state.send_gate_changed.wait (lock, [this] { return state.release_blocking_send; });
        }
        state.last_event = event.value;
    }

    reply_t handle_route_request (const request_t &request,
                                  const zlink::framework::route_message_context_t &context)
    {
        state.last_route_request = request.value;
        state.last_route_source = context.source_node_rid.to_string ();
        state.last_route_request_thread = std::this_thread::get_id ();
        state.route_request_saw_permit_release =
          state.application_permit_released != nullptr && *state.application_permit_released;
        return {request.value + 200};
    }

    void handle_route_send (const event_t &event,
                            const zlink::framework::route_message_context_t &context)
    {
        state.last_route_event = event.value;
        state.last_route_source = context.source_node_rid.to_string ();
        state.last_route_send_thread = std::this_thread::get_id ();
        state.route_send_saw_permit_release =
          state.application_permit_released != nullptr && *state.application_permit_released;
    }

    local_handler_state_t &state;
};

struct route_filter_state_t
{
    bool reject_request = false;
    bool suppress_send = false;
    std::vector<zlink::framework::handler_dispatch_kind_t> seen_kinds;
    std::string last_mesh;
    std::string last_channel;
};
} // namespace framework_channel_messaging_fixture
