/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::pubsub::client
{

inline void run_fanout_basic_delivery_scenario (const client_options_t &options)
{
    const auto &publisher_url = options.publisher_url;
    const auto urls = subscriber_urls (options);
    std::vector<bool> subscriber_ready (urls.size (), false);
    const auto warmup_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    for (int index = 0;
         std::chrono::steady_clock::now () < warmup_deadline
         && std::find (subscriber_ready.begin (), subscriber_ready.end (), false)
              != subscriber_ready.end ();
         ++index) {
        publish (publisher_url, topic_fanout, "warmup-" + std::to_string (index));
        evidence_wait_req_t request;
        request.contains_all_line_groups =
          {{"accepted|", "topic=" + std::string (topic_fanout), "value=warmup-"}};
        request.timeout_milliseconds = 100;
        for (std::size_t subscriber = 0; subscriber < urls.size (); ++subscriber) {
            if (!subscriber_ready[subscriber]) {
                subscriber_ready[subscriber] =
                  try_wait_for_subscriber_evidence (urls[subscriber], request).has_value ();
            }
        }
    }
    ensure (std::find (subscriber_ready.begin (), subscriber_ready.end (), false)
              == subscriber_ready.end (),
            "PS-A1 warm-up did not reach every subscriber");

    constexpr int measure_start = 100;
    constexpr int measure_count = 12;
    for (int index = measure_start; index < measure_start + measure_count; ++index) {
        publish (publisher_url, topic_fanout, "measure-" + std::to_string (index));
    }
    std::vector<std::vector<std::string>> required_prefix;
    for (int index = measure_start; index < measure_start + 3; ++index) {
        required_prefix.push_back (accepted_evidence ("measure-" + std::to_string (index)));
    }
    std::vector<std::vector<std::string>> snapshots;
    for (const auto &subscriber_url : urls) {
        snapshots.push_back (wait_for_subscriber_evidence (subscriber_url, required_prefix));
    }
    const auto shared_sequence = common_contiguous_sequence (
      snapshots, measure_start, measure_start + measure_count - 1);
    ensure (shared_sequence.size () >= 3,
            "PS-A1 expected a common contiguous sequence on all subscribers");
    std::cout << "scenario PS-A1 passed\n";
}

} // namespace zlink::framework::e2e::pubsub::client
