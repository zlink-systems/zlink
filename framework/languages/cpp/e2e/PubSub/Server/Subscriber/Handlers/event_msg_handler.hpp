/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Infrastructure/evidence_store.hpp"

#include <chrono>
#include <thread>

namespace zlink::framework::e2e::pubsub::server::subscriber
{

template <const char *Topic> class event_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<evidence_store_t>;
    using event_type = event_msg_t;
    static constexpr const char *topic_name = Topic;

    explicit event_handler_t (evidence_store_t &state) : _state (state) {}

    void handle (
      const event_msg_t &event,
      const zlink::framework::publish_message_context_t &context)
    {
        if (_state.handler_delay_ms > 0) {
            std::this_thread::sleep_for (std::chrono::milliseconds (_state.handler_delay_ms));
        }
        if (_state.accepts_topic (context.topic)) {
            _state.record_event (context.topic, event.value);
        } else {
            _state.record_ignored_event (context.topic, event.value);
        }
    }

  private:
    evidence_store_t &_state;
};

inline constexpr char fanout_topic_name[] = "fanout";
inline constexpr char alpha_topic_name[] = "alpha";
inline constexpr char beta_topic_name[] = "beta";

using fanout_handler_t = event_handler_t<fanout_topic_name>;
using alpha_handler_t = event_handler_t<alpha_topic_name>;
using beta_handler_t = event_handler_t<beta_topic_name>;

} // namespace zlink::framework::e2e::pubsub::server::subscriber
