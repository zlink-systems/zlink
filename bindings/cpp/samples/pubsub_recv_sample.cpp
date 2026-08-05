/* SPDX-License-Identifier: MPL-2.0 */

#include "sample_common.hpp"

int main ()
{
    // --8<-- [start:doc]
    zlink::context_t ctx;
    zlink::xpub_socket_t publisher (ctx);
    zlink::sub_socket_t subscriber (ctx);
    zlink::socket_monitor_t pub_monitor = publisher.monitor_open ();
    zlink::socket_monitor_t sub_monitor = subscriber.monitor_open ();

    publisher.bind ("tcp://127.0.0.1:0");
    const std::string endpoint = publisher.options ().last_endpoint ();
    assert (!endpoint.empty ());
    subscriber.connect (endpoint);
    assert (detail::wait_connected (pub_monitor, sub_monitor));

    const std::string topic = detail::k_pubsub_topic;
    subscriber.set_subscription (topic);

    zlink::subscription_event_t event;
    assert (publisher.receive_subscription_event (event)
            == static_cast<int> (zlink::recv_result_t::ok));
    assert (event.subscribed);
    assert (event.topic == topic);

    const std::string sent = detail::k_pubsub_payload;
    zlink::message_t outbound = detail::make_message (sent);
    publisher.publish (topic).message (outbound).submit ();

    zlink::topic_message_t inbound;
    assert (subscriber.subscribe (inbound) == static_cast<int> (zlink::recv_result_t::ok));
    assert (inbound.topic () == topic);
    assert (inbound.parts ().size () == 1);
    const std::string received = inbound.parts ()[0].to_string ();
    assert (received == detail::k_pubsub_payload);
    inbound.close ();
    std::printf ("[pubsub/recv] publish: \"%s/%s\" → subscribe: \"%s/%s\"\n", topic.c_str (),
                 sent.c_str (), topic.c_str (), received.c_str ());
    return 0;
    // --8<-- [end:doc]
}
