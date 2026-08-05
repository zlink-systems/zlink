/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::pubsub::client
{

inline void run_topic_filter_scenario (const client_options_t &options)
{
    const auto &publisher_url = options.publisher_url;
    for (int index = 0; index < 8; ++index) {
        publish (publisher_url, topic_alpha, "alpha-" + std::to_string (index));
        publish (publisher_url, topic_beta, "beta-" + std::to_string (index));
    }
    const auto urls = subscriber_urls (options);
    for (std::size_t subscriber = 0; subscriber < urls.size (); ++subscriber) {
        const bool accepts_alpha = subscriber != 1;
        std::vector<std::vector<std::string>> expected;
        for (int index = 0; index < 8; ++index) {
            expected.push_back (accepts_alpha
                                  ? accepted_evidence ("alpha-" + std::to_string (index), topic_alpha)
                                  : ignored_evidence ("alpha-" + std::to_string (index), topic_alpha));
            expected.push_back (accepts_alpha
                                  ? ignored_evidence ("beta-" + std::to_string (index), topic_beta)
                                  : accepted_evidence ("beta-" + std::to_string (index), topic_beta));
        }
        const auto lines = wait_for_subscriber_evidence (urls[subscriber], std::move (expected));
        ensure_no_evidence_line (
          lines, {"accepted|", "topic=" + std::string (accepts_alpha ? topic_beta : topic_alpha)},
          "PS-A2 subscriber accepted a filtered topic");
    }
    std::cout << "scenario PS-A2 passed\n";
}

} // namespace zlink::framework::e2e::pubsub::client
