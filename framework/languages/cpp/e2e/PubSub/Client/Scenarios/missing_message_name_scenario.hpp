/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::pubsub::client
{

inline void run_missing_message_name_scenario (const client_options_t &options)
{
    const auto &publisher_url = options.publisher_url;
    publish (publisher_url, topic_fanout, "missing", true);
    std::this_thread::sleep_for (std::chrono::milliseconds (500));
    publish (publisher_url, topic_fanout, "after-missing");
    const std::vector<std::vector<std::string>> expected{
      accepted_evidence ("after-missing"), dispatch_error_evidence ("MissingEventMsg")};
    for (const auto &subscriber_url : subscriber_urls (options)) {
        (void) wait_for_subscriber_evidence (subscriber_url, expected);
    }
    std::cout << "scenario PS-C1 passed\n";
}

} // namespace zlink::framework::e2e::pubsub::client
