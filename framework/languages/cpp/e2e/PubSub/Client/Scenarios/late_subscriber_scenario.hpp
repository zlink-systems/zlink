/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::pubsub::client
{

inline void run_late_subscriber_scenario (const client_options_t &options)
{
    const auto &publisher_url = options.publisher_url;
    for (int index = 0; index < 5; ++index) {
        publish (publisher_url, topic_fanout, "before-late-" + std::to_string (index));
    }
    touch_file (options.ready_file);
    wait_for_file (options.continue_file);
    std::this_thread::sleep_for (std::chrono::milliseconds (500));
    for (int index = 0; index < 8; ++index) {
        publish (publisher_url, topic_fanout, "after-late-" + std::to_string (index));
    }
    const auto urls = subscriber_urls (options);
    std::vector<std::vector<std::string>> early_expected;
    std::vector<std::vector<std::string>> late_expected;
    for (int index = 0; index < 5; ++index) {
        early_expected.push_back (accepted_evidence ("before-late-" + std::to_string (index)));
    }
    for (int index = 0; index < 8; ++index) {
        const auto marker = accepted_evidence ("after-late-" + std::to_string (index));
        early_expected.push_back (marker);
        late_expected.push_back (marker);
    }
    (void) wait_for_subscriber_evidence (urls[0], early_expected);
    (void) wait_for_subscriber_evidence (urls[1], early_expected);
    const auto late_lines = wait_for_subscriber_evidence (urls[2], late_expected);
    ensure_no_evidence_line (late_lines, {"accepted|", "value=before-late-"},
                             "PS-A3 late subscriber received a pre-join event");
    std::cout << "scenario PS-A3 passed\n";
}

} // namespace zlink::framework::e2e::pubsub::client
