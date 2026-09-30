/* SPDX-License-Identifier: FSL-1.1-ALv2 */

// The CS client process (§6.2, §6.1 Client): options, the scenario named by the cell id, and the runner's stdin/stdout
// JSON control pipe (§16). The measured connector calls live in the scenario headers.

#include "metrics_client.hpp"
#include "session_echo_only_scenario.hpp"

#include <iostream>

namespace
{
using namespace perf;

void reply (const json &value) { std::cout << value.dump () << std::endl; }
} // namespace

int main (int argc, char **argv)
{
    if (argc != 5 || std::string (argv[1]) != "--endpoint-config" || std::string (argv[3]) != "--client-index")
        throw std::invalid_argument ("Client requires --endpoint-config <file> --client-index <index>.");
    std::ifstream file (argv[2]);
    if (!file)
        throw std::invalid_argument (std::string ("Cannot read endpoint manifest ") + argv[2]);
    const auto manifest = json::parse (file).get<endpoint_manifest_t> ();
    const int index = std::stoi (argv[4]);
    if (index < 0 || index >= manifest.workload.client_count)
        throw std::out_of_range ("client index");
    const bool cs = std::any_of (manifest.roles.begin (), manifest.roles.end (), [] (const auto &role) { return role.stream_endpoint.has_value (); });

    role_config_t config;
    config.run_id = manifest.run_id;
    config.cell_id = manifest.cell_id;
    config.config_hash = manifest.config_hash;
    config.role = "client";
    config.role_instance = index;
    config.scenario = manifest.cell_id.substr (0, manifest.cell_id.find ('/'));
    config.object_role = "None";
    config.execution_mode = "Immediate";
    config.workload = manifest.workload;
    config.provenance = manifest.provenance;
    measurement_t measurement (config, cs);

    // Every CS cell shares the connector loop of the baseline; the cell id names which standard scenario runs.
    std::unique_ptr<session_echo_only_scenario_t> scenario;
    if (cs)
        scenario = std::make_unique<session_echo_only_scenario_t> (manifest, measurement, index);
    const metrics_client_t admin (manifest);
    if (scenario)
        scenario->prepare ();
    reply ({{"type", "prepared"}, {"ok", !measurement.has_errors ()}, {"snapshot", measurement.snapshot (nullptr)}});
    std::string line;
    while (std::getline (std::cin, line)) {
        try {
            const auto document = json::parse (line);
            const auto command = document.at ("command").get<std::string> ();
            json response;
            if (command == "start")
                response = measurement.start (document.at ("request").get<trigger_request_t> (),
                                              scenario ? workload_fn_t ([&] (const loops_t &loops) { scenario->run (loops); }) : workload_fn_t{});
            else if (command == "triggerRoles")
                response = admin.trigger_roles (document.at ("request").get<trigger_request_t> ());
            else if (command == "resetRoles")
                response = admin.reset_roles (document.at ("request").get<reset_request_t> ());
            else if (command == "reset")
                response = measurement.reset (document.at ("request").get<reset_request_t> (), nullptr).first;
            else if (command == "wait") {
                measurement.wait_phase ();
                response = {{"ok", !measurement.has_errors ()}, {"phase", measurement.phase ()}};
            }
            else if (command == "stats")
                response = measurement.snapshot (nullptr);
            else if (command == "stop")
                return 0;
            else
                throw std::invalid_argument ("Unknown control command.");
            reply ({{"ok", true}, {"response", response}});
        }
        catch (...) {
            const auto error = std::current_exception ();
            measurement.record_diagnostic (error);
            try {
                std::rethrow_exception (error);
            }
            catch (const std::exception &failure) {
                reply ({{"ok", false}, {"errorType", type_name (typeid (failure))}, {"message", failure.what ()}});
            }
            catch (...) {
                reply ({{"ok", false}, {"errorType", "unknown exception"}, {"message", "Non-standard exception."}});
            }
        }
    }
    return 0;
}
