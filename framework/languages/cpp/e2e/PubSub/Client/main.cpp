/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "Scenarios/fanout_basic_delivery_scenario.hpp"
#include "Scenarios/late_subscriber_scenario.hpp"
#include "Scenarios/missing_message_name_scenario.hpp"
#include "Scenarios/publisher_restart_scenario.hpp"
#include "Scenarios/slow_subscriber_scenario.hpp"
#include "Scenarios/subscriber_reconnect_scenario.hpp"
#include "Scenarios/topic_filter_scenario.hpp"
#include "Support/client_support.hpp"

#include <exception>
#include <iostream>
#include <string>

namespace ps_client = zlink::framework::e2e::pubsub::client;

int main (int argc, char **argv)
{
    try {
        const auto options = ps_client::read_client_options (argc, argv);
        ps_client::touch_file (options.start_ready_file);
        ps_client::wait_for_file (options.start_continue_file);
        std::this_thread::sleep_for (std::chrono::milliseconds (1000));

        const auto &scenario = options.scenario;
        ps_client::ensure (!options.publisher_url.empty (), "publisherUrl is required");

        if (scenario == "basic") {
            ps_client::run_fanout_basic_delivery_scenario (options);
        } else if (scenario == "topic") {
            ps_client::run_topic_filter_scenario (options);
        } else if (scenario == "late") {
            ps_client::run_late_subscriber_scenario (options);
        } else if (scenario == "reconnect") {
            ps_client::run_subscriber_reconnect_scenario (options);
        } else if (scenario == "slow") {
            ps_client::run_slow_subscriber_scenario (options);
        } else if (scenario == "publisher-restart") {
            ps_client::run_publisher_restart_scenario (options);
        } else if (scenario == "negative") {
            ps_client::run_missing_message_name_scenario (options);
        } else {
            throw std::runtime_error ("unknown scenario " + scenario);
        }

        std::cout << "pubsub e2e result=passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "pubsub scenario failed: " << error.what () << "\n";
        return 1;
    }
}
