import zlink
from sample_support import tcp_endpoint, wait_connected


def main():
# --8<-- [start:doc]
    _, endpoint = tcp_endpoint()

    with zlink.create_context() as ctx:
        with zlink.create_xpub_socket(ctx) as publisher:
            with zlink.create_sub_socket(ctx) as subscriber:
                with publisher.monitor_open(zlink.MonitorEventMask.CONNECTION_READY) as publisher_monitor:
                    with subscriber.monitor_open(zlink.MonitorEventMask.CONNECTION_READY) as subscriber_monitor:
                        publisher.bind(endpoint)
                        subscriber.connect(endpoint)
                        subscriber.set_subscription(b"prices")
                        wait_connected(publisher_monitor, subscriber_monitor)

                event = zlink.create_subscription_event()
                if not publisher.receive_subscription_event_into(event):
                    raise AssertionError("missing subscription event")
                if not event.subscribed or event.topic != "prices":
                    raise AssertionError("unexpected subscription event")
                publisher.publish(b"prices").message(b"101.25").submit()
                received = zlink.create_topic_message()
                assert subscriber.subscribe_into(received)
                with received:
                    if received.topic != "prices":
                        raise AssertionError(f"unexpected pubsub topic: {received.topic!r}")
                    if received.to_bytes_list() != [b"101.25"]:
                        raise AssertionError("unexpected pubsub payload")
                print('[pubsub/recv] publish: "prices/101.25" → subscribe: "prices/101.25"')
# --8<-- [end:doc]


if __name__ == "__main__":
    main()
