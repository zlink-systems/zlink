/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Configuration/subscriber_options.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace zlink::framework::e2e::pubsub::server::subscriber
{

class evidence_store_t
{
  public:
    evidence_store_t (std::string subscriber_id, std::string accepted_topics, int handler_delay_ms) :
        subscriber_id (std::move (subscriber_id)),
        accepted_topics (std::move (accepted_topics)),
        handler_delay_ms (handler_delay_ms)
    {
    }

    bool accepts_topic (const std::string &topic) const
    {
        return server::has_topic (accepted_topics, topic);
    }

    void record_event (std::string topic, std::string value)
    {
        std::lock_guard lock (_mutex);
        const auto topic_copy = topic;
        const auto value_copy = value;
        events.push_back ({subscriber_id, std::move (topic), std::move (value)});
        entries.push_back ("accepted|subscriber=" + subscriber_id + "|topic=" + topic_copy
                           + "|value=" + value_copy);
    }

    void record_ignored_event (std::string topic, std::string value)
    {
        std::lock_guard lock (_mutex);
        const auto topic_copy = topic;
        const auto value_copy = value;
        ignored_events.push_back ({subscriber_id, std::move (topic), std::move (value)});
        entries.push_back ("ignored|subscriber=" + subscriber_id + "|topic=" + topic_copy
                           + "|value=" + value_copy);
    }

    void record_error (const zlink::framework::message_flow_event_t &error)
    {
        if (error.outcome != zlink::framework::message_flow_outcome_t::error) {
            return;
        }
        std::string message;
        if (error.exception) {
            try {
                std::rethrow_exception (error.exception);
            }
            catch (const std::exception &ex) {
                message = ex.what ();
            }
            catch (...) {
                message = "unknown";
            }
        }
        std::lock_guard lock (_mutex);
        const auto message_kind = server::kind_name (error.message_kind);
        const auto reason = server::reason_name (*error.error_reason);
        const auto action = server::action_name (*error.error_action);
        const auto packet_name = error.packet_name.value_or ("");
        const auto topic = error.topic.value_or ("");
        errors.push_back (
          {message_kind, reason, action, packet_name, topic, std::move (message)});
        entries.push_back ("error|subscriber=" + subscriber_id + "|kind=" + message_kind
                           + "|reason=" + reason + "|action=" + action + "|packet="
                           + packet_name + "|topic=" + topic);
    }

    evidence_snapshot_t snapshot () const
    {
        std::lock_guard lock (_mutex);
        return {.subscriber_id = subscriber_id,
                .events = events,
                .ignored_events = ignored_events,
                .errors = errors};
    }

    std::vector<std::string> wait (const evidence_wait_req_t &request) const
    {
        const auto timeout =
          std::chrono::milliseconds (std::clamp (request.timeout_milliseconds, 1, 30000));
        const auto deadline = std::chrono::steady_clock::now () + timeout;
        do {
            auto snapshot = entries_snapshot ();
            if (matches (snapshot, request)) {
                return snapshot;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        } while (std::chrono::steady_clock::now () < deadline);

        throw std::runtime_error ("timed out waiting for PubSub evidence");
    }

    std::string subscriber_id;
    std::string accepted_topics;
    int handler_delay_ms = 0;

  private:
    std::vector<std::string> entries_snapshot () const
    {
        std::lock_guard lock (_mutex);
        return entries;
    }

    static bool contains (const std::string &entry, const std::string &expected)
    {
        return entry.find (expected) != std::string::npos;
    }

    static bool any_entry_contains (const std::vector<std::string> &snapshot,
                                    const std::string &expected)
    {
        for (const auto &entry : snapshot) {
            if (contains (entry, expected)) {
                return true;
            }
        }
        return false;
    }

    static bool any_entry_contains_all (const std::vector<std::string> &snapshot,
                                        const std::vector<std::string> &group)
    {
        for (const auto &entry : snapshot) {
            bool matched = true;
            for (const auto &expected : group) {
                if (!contains (entry, expected)) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                return true;
            }
        }
        return false;
    }

    static bool matches (const std::vector<std::string> &snapshot,
                         const evidence_wait_req_t &request)
    {
        for (const auto &expected : request.contains_all) {
            if (!any_entry_contains (snapshot, expected)) {
                return false;
            }
        }
        for (const auto &group : request.contains_any_groups) {
            bool matched = group.empty ();
            for (const auto &expected : group) {
                if (any_entry_contains (snapshot, expected)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                return false;
            }
        }
        for (const auto &group : request.contains_all_line_groups) {
            if (!any_entry_contains_all (snapshot, group)) {
                return false;
            }
        }
        if (!request.contains_any_line_groups.empty ()) {
            bool matched = false;
            for (const auto &group : request.contains_any_line_groups) {
                if (any_entry_contains_all (snapshot, group)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                return false;
            }
        }
        return true;
    }

    mutable std::mutex _mutex;
    std::vector<evidence_event_t> events;
    std::vector<evidence_event_t> ignored_events;
    std::vector<dispatch_error_evidence_t> errors;
    std::vector<std::string> entries;
};

} // namespace zlink::framework::e2e::pubsub::server::subscriber
