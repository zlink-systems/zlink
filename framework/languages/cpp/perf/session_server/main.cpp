/* SPDX-License-Identifier: FSL-1.1-ALv2 */

// The Session receiver roles of §11.1 (STREAM only: no Object Server, Actor, Store or automatic discovery) and §10.2
// (an Object Client node; the Actors live in the separate Actor process).

#include <perf/server/actor_echo_support.hpp>

namespace
{
using namespace perf;

// §11.1: the one public session callback; a typed PerfEchoRequest is validated and answered with the typed echo
// (connector `request(dto).submit<PerfEchoReply>(...)` on the other side).
class perf_session_t final : public fw::packet_stream_session_t
{
  public:
    explicit perf_session_t (role_t &role) : _role (role) {}

    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &error) override
    {
        _role.measurement.record_diagnostic (std::make_exception_ptr (
          std::runtime_error (std::string ("STREAM ") + std::string (error.message ()))));
        co_return;
    }

    fw::task_t<void> on_packet (fw::stream_t &stream, const fw::session_message_context_t &, const zlink::message_t &payload) override
    {
        const auto received = now_ticks ();
        auto &measurement = _role.measurement;
        const handler_scope_t scope (measurement);
        try {
            const auto request = payload.parse_json<echo_request_t> ();
            measurement.validate_request (request);
            const auto reply = payload_pattern_t::reply (request, received);
            measurement.record_reply (request);
            co_await stream.reply_packet (zlink::message_t::from_json (reply)).async ();
            if (measurement.phase () == "setup")
                measurement.set_setup_evidence (json::array ({{{"kind", "typedProbeReply"}, {"source", "perf_session_t::on_packet stream.reply_packet.async"},
                                                               {"observedValue", request.correlation_id}}}));
        }
        catch (...) {
            measurement.record_diagnostic (std::current_exception ());
            throw;
        }
    }

  private:
    role_t &_role;
};
} // namespace

int main (int argc, char **argv)
{
    auto config = perf::read_role_config (argc, argv);
    if (config.role != "session" || config.source || (config.scenario != "session-echo-only" && config.scenario != "cs-remote-session-actor-echo"))
        throw std::invalid_argument ("SessionServer supports the session receiver roles of §10.2 and §11.1.");
    const bool baseline = config.scenario == "session-echo-only";
    auto role = std::make_unique<perf::role_t> (std::move (config), false);
    const auto &settings = role->config;
    const auto stream_endpoint = settings.transport_endpoints.at ("stream");
    return perf::run_role (std::move (role), [&] (fw::zlink_framework_options_t &options, fw::app_t &) {
        if (baseline) {
            options.add_stream_node ("perf-session").bind (stream_endpoint).register_session<perf_session_t> ();
            return;
        }
        // §10.2: an Object Client node; the Actors live in the separate Actor process.
        auto mesh = options.add_route_mesh (*settings.mesh_name);
        mesh.set_automatic_routing_id_prefix ("perf-session");
        mesh.listen (settings.transport_endpoints.at ("mesh"));
        mesh.objects ().client ();
        options.services ().add_singleton<session_actor_setup_t, role_t> ();
        options.add_stream_node ("perf-session").bind (stream_endpoint).enable_actor_dispatch ().register_session<perf_actor_relay_session_t> ();
    });
}
