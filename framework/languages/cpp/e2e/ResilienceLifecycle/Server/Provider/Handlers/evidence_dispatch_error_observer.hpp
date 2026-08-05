/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Infrastructure/evidence_store.hpp"
#include "../Infrastructure/fault_state.hpp"

#include <zlink/framework.hpp>

#include <memory>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::resilience_lifecycle::provider
{

inline std::string dispatch_reason_name (zlink::framework::dispatch_error_reason_t reason)
{
    switch (reason) {
        case zlink::framework::dispatch_error_reason_t::handler_missing:
            return "handler_missing";
        case zlink::framework::dispatch_error_reason_t::payload_decode_failed:
            return "payload_decode_failed";
        case zlink::framework::dispatch_error_reason_t::handler_exception:
            return "handler_exception";
        case zlink::framework::dispatch_error_reason_t::invalid_frame:
            return "invalid_frame";
        case zlink::framework::dispatch_error_reason_t::reply_path_missing:
            return "reply_path_missing";
        case zlink::framework::dispatch_error_reason_t::unexpected_reply:
            return "unexpected_reply";
    }
    return "unknown";
}

inline std::string dispatch_action_name (zlink::framework::dispatch_error_action_t action)
{
    switch (action) {
        case zlink::framework::dispatch_error_action_t::drop:
            return "drop";
        case zlink::framework::dispatch_error_action_t::reply_error:
            return "reply_error";
    }
    return "unknown";
}

inline void configure_evidence_dispatch_error_observer (
  zlink::framework::zlink_framework_options_t &framework,
  std::shared_ptr<evidence_store_t> evidence,
  std::shared_ptr<fault_state_t> fault_state)
{
    framework.configure_dispatch ().set_message_flow_observer (
      [evidence = std::move (evidence),
       fault_state = std::move (fault_state)] (const zlink::framework::message_flow_event_t &event) {
          if (event.outcome != zlink::framework::message_flow_outcome_t::error
              || !event.error_reason || !event.error_action) {
              return;
          }
          evidence->record ("DispatchError",
                            dispatch_reason_name (*event.error_reason) + ":"
                              + dispatch_action_name (*event.error_action));
          if (fault_state->mode () == "observer-throws") {
              throw std::runtime_error ("dispatch observer failure");
          }
      });
}

} // namespace zlink::framework::e2e::resilience_lifecycle::provider
