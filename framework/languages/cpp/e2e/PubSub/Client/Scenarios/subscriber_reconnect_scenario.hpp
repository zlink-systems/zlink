/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::pubsub::client
{

inline void run_subscriber_reconnect_scenario (const client_options_t &options)
{
    const auto &publisher_url = options.publisher_url;
    const auto &reconnect_url = options.reconnect_subscriber_url;
    ensure (!reconnect_url.empty (), "reconnectSubscriberUrl is required");
    auto reconnect_subscriber =
      start_subscriber_process (options, "sub-reconnect", reconnect_url, "sub-3", "fanout", "fanout");
    write_pid_file (options.reconnect_subscriber_pid_file,
                    reconnect_subscriber.pid ());

    for (int index = 0; index < 5; ++index) {
        publish (publisher_url, topic_fanout, "before-reconnect-" + std::to_string (index));
    }
    reconnect_subscriber.terminate ();
    wait_http_health ("sub-reconnect", reconnect_url, false);

    for (int index = 0; index < 5; ++index) {
        publish (publisher_url, topic_fanout, "during-reconnect-" + std::to_string (index));
    }
    reconnect_subscriber =
      start_subscriber_process (options, "sub-reconnect", reconnect_url, "sub-3", "fanout", "fanout");
    write_pid_file (options.reconnect_subscriber_pid_file,
                    reconnect_subscriber.pid ());

    for (int index = 0; index < 8; ++index) {
        publish (publisher_url, topic_fanout, "after-reconnect-" + std::to_string (index));
    }
    const auto urls = subscriber_urls (options);
    std::vector<std::vector<std::string>> stable_expected;
    std::vector<std::vector<std::string>> rejoined_expected;
    for (int index = 0; index < 5; ++index) {
        stable_expected.push_back (
          accepted_evidence ("during-reconnect-" + std::to_string (index)));
    }
    for (int index = 0; index < 8; ++index) {
        const auto marker = accepted_evidence ("after-reconnect-" + std::to_string (index));
        stable_expected.push_back (marker);
        rejoined_expected.push_back (marker);
    }
    (void) wait_for_subscriber_evidence (urls[0], stable_expected);
    (void) wait_for_subscriber_evidence (urls[1], stable_expected);
    const auto rejoined_lines = wait_for_subscriber_evidence (urls[2], rejoined_expected);
    ensure_no_evidence_line (rejoined_lines, {"accepted|", "value=during-reconnect-"},
                             "PS-A4 rejoined subscriber received a disconnect-gap event");
    std::cout << "scenario PS-A4 passed\n";
}

} // namespace zlink::framework::e2e::pubsub::client
