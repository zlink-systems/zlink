/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace fw = zlink::framework;
using namespace std::chrono_literals;

namespace
{

struct options_t
{
    std::string role;
    std::string redis_endpoint;
    std::string key_prefix;
    std::filesystem::path trigger_file;
    std::filesystem::path stop_file;
};

std::string argument_value (int argc, char **argv, std::string_view name)
{
    const auto prefix = std::string (name) + "=";
    for (int index = 1; index < argc; ++index) {
        const std::string argument (argv[index]);
        if (argument.starts_with (prefix))
            return argument.substr (prefix.size ());
    }
    throw std::invalid_argument ("missing argument " + std::string (name));
}

options_t read_options (int argc, char **argv)
{
    options_t options{argument_value (argc, argv, "--role"), argument_value (argc, argv, "--redis"),
                      argument_value (argc, argv, "--key-prefix"),
                      argument_value (argc, argv, "--trigger-file"),
                      argument_value (argc, argv, "--stop-file")};
    if (options.role != "source" && options.role != "target")
        throw std::invalid_argument ("role must be source or target");
    return options;
}

bool wait_until (const std::function<bool ()> &condition, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (condition ())
            return true;
        std::this_thread::sleep_for (10ms);
    }
    return condition ();
}

void require_result (const fw::relocation_result_t &result,
                     fw::relocation_outcome_t outcome,
                     fw::relocation_reason_t reason,
                     std::string_view label)
{
    std::cout << label << " outcome=" << static_cast<int> (result.outcome)
              << " reason=" << static_cast<int> (result.reason) << '\n'
              << std::flush;
    if (result.outcome != outcome || result.reason != reason)
        throw std::runtime_error (std::string (label) + " returned an unexpected terminal");
}

} // namespace

int main (int argc, char **argv)
{
    try {
        const auto options = read_options (argc, argv);
        auto app = fw::app_t::create ();
        app.add_zlink_framework ([&] (fw::zlink_framework_options_t &framework) {
            framework.add_location_store (std::make_shared<fw::redis::redis_location_store_t> (
              fw::redis::redis_location_options_t{.connection_string = options.redis_endpoint,
                                                  .key_prefix = options.key_prefix}));
            framework.add_relocation_store (std::make_shared<fw::redis::redis_relocation_store_t> (
              fw::redis::redis_relocation_options_t{.connection_string = options.redis_endpoint,
                                                    .key_prefix =
                                                      options.key_prefix + ":relocation"}));
            framework.configure_locations ().polling_interval = 25ms;
            auto mesh = framework.add_route_mesh ("relocation-retry-mesh");
            mesh.channel_name ("relocation-retry-channel").server ();
            mesh.listen ("tcp://127.0.0.1:0")
              .set_routing_id (zlink::routing_id_t::from ("relocation-retry-" + options.role));
        });

        char program[] = "relocation-retry-role";
        char *arguments[] = {program, nullptr};
        int exit_code = -1;
        std::thread app_thread ([&] { exit_code = app.run (1, arguments); });
        if (!wait_until ([&] { return app.is_ready (); }, 5s)) {
            app.request_stop ();
            app_thread.join ();
            throw std::runtime_error ("role did not reach Serving");
        }
        std::cout << "role=" << options.role << " state=serving\n" << std::flush;

        if (options.role == "source") {
            const auto first =
              app.relocate ({.mode = fw::relocation_mode_t::planned_maintenance, .deadline = 300ms})
                .result ()
                .value ();
            require_result (first, fw::relocation_outcome_t::blocked,
                            fw::relocation_reason_t::target_unavailable, "first-relocation");
            if (!app.is_ready ())
                throw std::runtime_error ("blocked relocation did not preserve Serving");
            if (!wait_until ([&] { return std::filesystem::exists (options.trigger_file); }, 15s)) {
                throw std::runtime_error ("timed out waiting for retry trigger");
            }
            std::cout << "second-relocation state=waiting-for-target\n" << std::flush;
            const auto second =
              app.relocate ({.mode = fw::relocation_mode_t::planned_maintenance, .deadline = 5s})
                .result ()
                .value ();
            require_result (second, fw::relocation_outcome_t::relocated,
                            fw::relocation_reason_t::none, "second-relocation");
        } else if (!wait_until ([&] { return std::filesystem::exists (options.stop_file); }, 20s)) {
            throw std::runtime_error ("timed out waiting for stop trigger");
        }

        const auto stopped = app.shutdown (3s).result ().value ();
        app_thread.join ();
        if (stopped.outcome != fw::termination_outcome_t::stopped || exit_code != 0)
            throw std::runtime_error ("role did not stop cleanly");
        std::cout << "role=" << options.role << " result=passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "relocation retry role failed: " << error.what () << '\n';
        return 1;
    }
}
