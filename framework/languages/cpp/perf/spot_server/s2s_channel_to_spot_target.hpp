/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// The Spot process of §10.3 and §10.4 (Object Server, no driver): User Spots that answer a request with a typed echo
// (perf_echo_spot_t, §10.3) or answer a send by sending the echo to the caller return Channel (s2s_send_echo_spot_t, §10.4).
// The measured operation lives in the Channel process; this side only echoes.

#include <perf/server/spot_role.hpp>

namespace perf
{
// §10.4: the echo goes back as a second one-way send to the caller own return ChannelName (in the DTO).
class s2s_send_echo_spot_t final : public perf_spot_base_t<s2s_send_echo_spot_t>
{
  public:
    s2s_send_echo_spot_t (fw::spot_context_t context, role_t &role, fw::route_client_t &route) :
        perf_spot_base_t (std::move (context)), _role (role), _route (route)
    {
    }
    void configure () override { _context.handlers ().add_handler<&s2s_send_echo_spot_t::echo> (echo_request_t::packet_name); }

    fw::task_t<void> echo (const echo_request_t &message)
    {
        const auto received = now_ticks ();
        auto &measurement = _role.measurement;
        const handler_scope_t scope (measurement);
        try {
            measurement.validate_request (message, message.return_channel);
            if (!message.return_channel || message.return_channel->empty ())
                throw validation_error_t ("IdentityMismatch", "No return Channel in the request.");
            const auto reply = payload_pattern_t::reply (message, received);
            measurement.record_application_call (message, "send");
            co_await _route.send_to_channel (*message.return_channel, reply).async ();
            if (measurement.phase () == "setup")
                measurement.set_setup_evidence (json::array ({{{"kind", "typedProbeReply"}, {"source", "Spot packet handler -> send_to_channel"},
                                                               {"observedValue", message.correlation_id}}}));
        }
        catch (...) {
            measurement.record_diagnostic (std::current_exception ());
            throw;
        }
    }

  private:
    role_t &_role;
    fw::route_client_t &_route;
};

// The Object Server side of setup: the Spots exist and are active; the source Channel process does the probing.
inline void prepare_spot_target (role_t &role, const std::atomic<bool> &stopping)
{
    publish_spots (role, create_spots (role, stopping));
}
} // namespace perf
