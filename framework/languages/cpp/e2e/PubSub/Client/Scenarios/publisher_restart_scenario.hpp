/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::pubsub::client
{

inline void run_publisher_restart_scenario (const client_options_t &options)
{
    const auto &publisher_url = options.publisher_url;
    publish (publisher_url, topic_fanout, "before-publisher-restart-1");

    post_empty (publisher_url, "/shutdown");
    wait_http_health ("publisher", publisher_url, false);
    ensure (!try_post_empty (publisher_url,
                             "/publish/event?topic=" + url_encode (topic_fanout)
                               + "&value=during-publisher-down"),
            "PS-B2 expected publish attempt to fail while publisher is down");

    auto restarted_publisher = start_publisher_process (options, "pub-restart", publisher_url);
    write_pid_file (options.restarted_publisher_pid_file,
                    restarted_publisher.pid ());
    const auto urls = subscriber_urls (options);
    std::vector<bool> subscriber_ready (urls.size (), false);
    const auto warmup_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (3);
    for (int index = 0;
         std::chrono::steady_clock::now () < warmup_deadline
         && std::find (subscriber_ready.begin (), subscriber_ready.end (), false)
              != subscriber_ready.end ();
         ++index) {
        publish (publisher_url, topic_fanout,
                 "publisher-restart-warmup-" + std::to_string (index));
        evidence_wait_req_t request;
        request.contains_all_line_groups =
          {{"accepted|", "topic=" + std::string (topic_fanout),
            "value=publisher-restart-warmup-"}};
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
            "PS-B2 warm-up did not reach every subscriber after publisher restart");

    for (int index = 20; index <= 42; ++index) {
        publish (publisher_url, topic_fanout,
                 "after-publisher-restart-" + std::to_string (index));
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    std::vector<std::vector<std::string>> snapshots;
    for (const auto &subscriber_url : urls) {
        snapshots.push_back (wait_for_subscriber_evidence (
          subscriber_url,
          {accepted_evidence ("after-publisher-restart-20"),
           accepted_evidence ("after-publisher-restart-21"),
           accepted_evidence ("after-publisher-restart-22")}));
    }
    const auto shared_sequence = common_contiguous_sequence (snapshots, 20, 42,
                                                             "after-publisher-restart-");
    ensure (shared_sequence.size () >= 3,
            "PS-B2 expected a common contiguous sequence after publisher restart");
    std::cout << "scenario PS-B2 passed\n";
}

} // namespace zlink::framework::e2e::pubsub::client
