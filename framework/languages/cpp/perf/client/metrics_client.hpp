/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// Only the standalone application client sends phase triggers and reset requests to the roles (§4.2, §16). HTTP
// acknowledgements are not echo operations. The client owns no Framework host, so this uses the http_client package.

#include <perf/config.hpp>

#include <zlink/http_client.hpp>

namespace perf
{
class metrics_client_t
{
  public:
    explicit metrics_client_t (const endpoint_manifest_t &manifest) : _manifest (manifest) {}

    // Manifest order is receivers then source, fixed by the coordinator before processes start.
    json trigger_roles (const trigger_request_t &request) const
    {
        json acknowledgements = json::array ();
        for (const auto &role : _manifest.roles) {
            const auto sent = now_ticks ();
            const auto body = post (role.application_trigger_url, json (request).dump (), "Trigger " + role.role);
            const auto received = now_ticks ();
            const auto ack = json::parse (body).get<trigger_reply_t> ();
            if (!ack.accepted || ack.run_id != request.run_id || ack.cell_id != request.cell_id || ack.reset_seq != request.reset_seq
                || ack.phase != request.phase || ack.config_hash != _manifest.config_hash)
                throw validation_error_t ("PhaseMismatch", "Role trigger acknowledgement identity differs.");
            acknowledgements.push_back ({{"role", role.role}, {"roleInstance", role.role_instance}, {"sentTicks", dec (sent)},
                                         {"ackTicks", dec (received)}, {"clockDomainId", clock_domain ()}, {"acknowledgement", ack}});
        }
        return acknowledgements;
    }

    json reset_roles (const reset_request_t &request) const
    {
        json acknowledgements = json::array ();
        for (const auto &role : _manifest.roles) {
            const auto body = post (role.metrics_base_url + "/perf/reset", json (request).dump (), "Reset " + role.role);
            const auto ack = json::parse (body);
            if (!ack.at ("ok").get<bool> () || ack.at ("runId") != request.run_id || ack.at ("cellId") != request.cell_id
                || ack.at ("resetSeq") != request.reset_seq)
                throw validation_error_t ("PhaseMismatch", "Role reset acknowledgement identity differs.");
            acknowledgements.push_back (ack);
        }
        return acknowledgements;
    }

  private:
    std::string post (const std::string &url, const std::string &body, const std::string &what) const
    {
        const auto path_start = url.find ('/', std::string ("http://").size ());
        auto client = zlink::http_client::client_t::create (url.substr (0, path_start))
                        .timeout (std::chrono::milliseconds (_manifest.workload.admin_timeout_ms))
                        .build ();
        const auto result = client.post (url.substr (path_start)).body (body, "application/json").submit_raw ();
        if (!result.has_value ())
            throw std::runtime_error (what + ": " + result.error ()->what ());
        const auto &response = result.value ();
        if (response.status < 200 || response.status >= 300)
            throw std::runtime_error (what + ": HTTP " + std::to_string (response.status) + ": " + response.body);
        return response.body;
    }

    const endpoint_manifest_t &_manifest;
};
} // namespace perf
