/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <perf/server/server_application.hpp>

namespace perf
{
// The Channel target of §10.6 (Object Client): the Channel send handler receives the Spot send and answers with a second
// one-way send to the SpotId that the DTO names in returnSpotId. The measured operation lives in the Spot process; this side
// does not assume the Channel context carries the source SpotId.
class s2s_return_to_spot_handler_t
{
  public:
    using message_type = echo_request_t;
    s2s_return_to_spot_handler_t (role_t &role, fw::route_client_t &route) : _role (role), _route (route) {}

    fw::task_t<void> handle (const echo_request_t &message)
    {
        const auto received = now_ticks ();
        auto &measurement = _role.measurement;
        const handler_scope_t scope (measurement);
        try {
            if (_role.config.spot_ids.empty ())
                throw validation_error_t ("IdentityMismatch", "No configured return SpotId for the client.");
            measurement.validate_request (message, std::nullopt,
                                          _role.config.spot_ids[static_cast<std::size_t> (message.client_id) % _role.config.spot_ids.size ()]);
            const auto reply = payload_pattern_t::reply (message, received);
            measurement.record_application_call (message, "send");
            co_await _route.send_to_spot (*message.return_spot_id, reply).async ();
            if (measurement.phase () == "setup")
                measurement.set_setup_evidence (json::array ({{{"kind", "typedProbeReply"}, {"source", "Channel send handler -> route_client_t.send_to_spot"},
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

} // namespace perf
