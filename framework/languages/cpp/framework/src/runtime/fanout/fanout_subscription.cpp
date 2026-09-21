/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/fanout/fanout_subscription.hpp"

#include <zlink/Contracts/Sockets/pubsub_socket_contracts.hpp>
#include <zlink/framework/contracts/channels/detail/fanout_topic.hpp>

#include <set>

namespace zlink::framework::runtime::fanout
{

std::vector<std::string>
fanout_subscription_topics (const std::vector<std::string> &application_topics)
{
    std::set<std::string> topics (application_topics.begin (), application_topics.end ());
    if (topics.empty ()) {
        topics.emplace ();
    }
    topics.emplace (detail::fanout_liveness_topic);
    return {topics.begin (), topics.end ()};
}

void apply_fanout_subscriptions (zlink::sub_socket_t &socket,
                                 const std::vector<std::string> &application_topics)
{
    for (const auto &topic : fanout_subscription_topics (application_topics)) {
        socket.set_subscription (topic);
    }
}

} // namespace zlink::framework::runtime::fanout
