/* SPDX-License-Identifier: FSL-1.1-ALv2 */

// The Subscriber role of §10.11: automatic Classic fanout with no endpoint and no subscribe(topic), so this subscriber receives
// every topic.

#include "fanout_receipts.hpp"

int main (int argc, char **argv)
{
    auto config = perf::read_role_config (argc, argv);
    if (config.scenario != "pubsub-fanout-echo" || config.role != "subscriber" || config.source)
        throw std::invalid_argument ("SubscriberServer runs the subscriber role of pubsub-fanout-echo.");
    auto role = std::make_unique<perf::role_t> (std::move (config), true);
    role->objects->set (false, "No Ready publisher is visible to this Subscriber yet.", perf::json::array ());
    auto &role_ref = *role;
    auto receipts = std::make_unique<perf::fanout_receipts_t> (role_ref, perf::cell_directory_of (argv[2]));
    auto &receipts_ref = *receipts;
    return perf::run_role (std::move (role), [&, receipts = std::move (receipts)] (fw::zlink_framework_options_t &options, fw::app_t &app) mutable {
        options.services ().add_singleton<perf::fanout_receipts_t> (std::move (receipts));
        options.handlers ().group ("perf-fanout").add_publish<perf::perf_fanout_handler_t> ();
        options.add_fanout_channel (*role_ref.config.channel_name).enable_subscriber ().add_handler_group ("perf-fanout");
        app.add_hosted_service (std::make_unique<perf::prepare_service_t> (role_ref, [&receipts_ref] (const std::atomic<bool> &stopping) { receipts_ref.prepare (stopping); }));
    });
}
