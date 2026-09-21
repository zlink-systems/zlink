/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <string>
#include <vector>

namespace zlink
{
class sub_socket_t;
}

namespace zlink::framework::runtime::fanout
{

std::vector<std::string>
fanout_subscription_topics (const std::vector<std::string> &application_topics);

void apply_fanout_subscriptions (zlink::sub_socket_t &socket,
                                 const std::vector<std::string> &application_topics);

} // namespace zlink::framework::runtime::fanout
