/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/client_support.hpp"

#include <future>
#include <iostream>

namespace zlink::framework::e2e::pubsub::client
{

inline void run_slow_subscriber_scenario (const client_options_t &options)
{
    const auto &publisher_url = options.publisher_url;
    for (int index = 0; index < 16; ++index) {
        publish (publisher_url, topic_fanout, "slow-isolation-" + std::to_string (index));
    }
    std::vector<std::vector<std::string>> expected;
    for (int index = 0; index < 16; ++index) {
        expected.push_back (accepted_evidence ("slow-isolation-" + std::to_string (index)));
    }
    const auto urls = subscriber_urls (options);
    const auto fast_wait_started = std::chrono::steady_clock::now ();
    auto fast_subscriber_two = std::async (std::launch::async, [&] {
        return wait_for_subscriber_evidence (urls[1], expected, 2000);
    });
    auto fast_subscriber_three = std::async (std::launch::async, [&] {
        return wait_for_subscriber_evidence (urls[2], expected, 2000);
    });
    (void) fast_subscriber_two.get ();
    (void) fast_subscriber_three.get ();
    const auto fast_wait_elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
      std::chrono::steady_clock::now () - fast_wait_started);
    ensure (fast_wait_elapsed <= std::chrono::milliseconds (2500),
            "PS-B1 fast subscriber evidence exceeded 2500 ms");
    (void) wait_for_subscriber_evidence (urls[0], expected);
    std::cout << "scenario PS-B1 passed\n";
}

} // namespace zlink::framework::e2e::pubsub::client
