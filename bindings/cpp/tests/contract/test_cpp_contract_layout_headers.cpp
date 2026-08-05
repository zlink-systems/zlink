/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink.hpp>
#include <zlink/Contracts/Core/capability.hpp>
#include <zlink/Contracts/Core/context_options.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Core/utilities.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Eventing/poll_event.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Messaging/subscription_event.hpp>
#include <zlink/Contracts/Messaging/topic_message.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/pubsub_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/socket_contracts.hpp>
#include <zlink/Contracts/Sockets/socket_options.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>

#include <type_traits>

static_assert (std::is_class<zlink::context_t>::value, "public projection must expose context_t");
static_assert (std::is_class<zlink::context_options_t>::value,
               "context_options.hpp must own context_options_t");
static_assert (std::is_class<zlink::message_t>::value, "public projection must expose message_t");
static_assert (std::is_class<zlink::received_t>::value, "public projection must expose received_t");
static_assert (std::is_class<zlink::topic_message_t>::value,
               "topic_message.hpp must own topic_message_t");
static_assert (std::is_class<zlink::subscription_event_t>::value,
               "subscription_event.hpp must own subscription_event_t");
static_assert (std::is_class<zlink::monitor_status_t>::value,
               "public projection must expose monitor_status_t");
static_assert (std::is_class<zlink::dealer_socket_t>::value,
               "public projection must expose dealer_socket_t");
static_assert (std::is_class<zlink::router_socket_t>::value,
               "public projection must expose router_socket_t");
static_assert (std::is_class<zlink::socket_monitor_t>::value,
               "public projection must expose socket_monitor_t");
static_assert (std::is_class<zlink::timer_t>::value, "public projection must expose timer_t");
static_assert (std::is_class<zlink::atomic_counter_t>::value,
               "public projection must expose atomic_counter_t");
static_assert (std::is_class<zlink::stopwatch_t>::value,
               "public projection must expose stopwatch_t");
static_assert (std::is_class<zlink::thread_t>::value,
               "public projection must expose thread_t");
static_assert (std::is_enum<zlink::poll_event_flag_t>::value,
               "poll_event.hpp must own poll_event_flag_t");
static_assert (std::is_class<zlink::poll_event_t>::value, "poll_event.hpp must own poll_event_t");
static_assert (std::is_class<zlink::poll_item_t>::value, "poller.hpp must own poll_item_t");
int main ()
{
    return 0;
}
