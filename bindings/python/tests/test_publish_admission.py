# SPDX-License-Identifier: MPL-2.0

import ctypes
import errno

import pytest
import zlink


@pytest.mark.parametrize("nonblocking", [False, True])
def test_nodrop_reports_backpressure_on_full_subscription_pipe(nonblocking):
    with (
        zlink.create_context() as context,
        zlink.create_xpub_socket(context) as publisher,
        zlink.create_xsub_socket(context) as subscriber,
        zlink.create_topic_message() as received,
    ):
        for socket in (publisher, subscriber):
            socket.options.linger_ms = 0
            socket.options.receive_timeout_ms = 1000
        publisher.options.send_high_water_mark = 4096
        subscriber.options.receive_high_water_mark = 4096
        publisher.bind("inproc://publish-admission")
        subscriber.connect("inproc://publish-admission")
        subscriber.set_subscription("topic")

        event = zlink.create_subscription_event()
        assert publisher.receive_subscription_event_into(event)
        assert (event.topic, event.subscribed) == ("topic", True)

        with zlink.Message.from_(b"caller owned") as source:
            publisher.publish("topic").messages(source, b"").submit()
            assert source.to_bytes() == b""
        assert subscriber.subscribe_into(received)
        assert received.to_bytes_list() == [b"caller owned", b""]
        received.close()

        payload = b"x" * 1024
        publisher.pub_options.no_drop = True
        publisher.options.send_timeout_ms = 20
        for accepted in range(64):
            operation = publisher.publish("topic").message(payload)
            if nonblocking:
                operation.flags(zlink.SendFlags.DONT_WAIT)
            try:
                operation.submit()
            except zlink.SubmitError as error:
                assert error.result is zlink.SubmitResult.BACKPRESSURED
                assert error.internal_errno == errno.EAGAIN
                assert accepted > 0
                break
        else:
            pytest.fail("NODROP did not report the full subscriber pipe")

        publisher.pub_options.no_drop = False
        assert publisher.publish("topic").message(b"dropped").submit() is None
        for _ in range(accepted):
            assert subscriber.subscribe_into(received)
            assert received.to_bytes_list() == [payload]
        assert subscriber.subscribe_into(received, flags=zlink.RecvFlags.DONT_WAIT) is False



@pytest.mark.parametrize("factory", [zlink.create_pub_socket, zlink.create_xpub_socket])
def test_publish_without_subscribers_succeeds(factory):
    with zlink.create_context() as context, factory(context) as publisher:
        publisher.pub_options.no_drop = True
        assert publisher.publish("unmatched").message(b"discarded").submit() is None
        with zlink.Message.from_(b"caller owned") as payload:
            assert publisher.publish("unmatched").messages(payload, b"").submit() is None
            assert payload.to_bytes() == b""


def test_publish_projects_core_not_connected(monkeypatch):
    from zlink._native.ffi import lib
    from zlink._runtime.sockets.socket_base_impl import _native_extension

    # Core publishes successfully with no subscribers. Inject NOT_CONNECTED
    # at the native boundary to exercise its projection on the Message path.
    native = lib()
    calls = []

    def reject_publish(topic, parts, count):
        calls.append((topic, count))
        for index in range(count):
            assert native.zlink_msg_close(ctypes.byref(parts[index])) == 0
        return int(zlink.SubmitResult.NOT_CONNECTED)

    if _native_extension is None:
        monkeypatch.setattr(
            native, "zlink_publish",
            lambda handle, topic, parts, count, flags: reject_publish(topic, parts, count),
        )
        monkeypatch.setattr(native, "zlink_errno", lambda: errno.ENOTCONN)
    else:
        def submit_storage(handle, target, parts, flags, context, timeout, token, topic):
            return reject_publish(topic, parts, len(parts)), errno.ENOTCONN, 0

        monkeypatch.setattr(_native_extension, "submit_storage", submit_storage)
    with (
        zlink.create_context() as context,
        zlink.create_pub_socket(context) as publisher,
        zlink.Message.from_(b"payload") as payload,
    ):
        with pytest.raises(zlink.SubmitError) as error:
            publisher.publish("topic").message(payload).submit()
        assert calls == [(b"topic", 1)]
        assert error.value.result is zlink.SubmitResult.NOT_CONNECTED
        assert error.value.internal_errno == errno.ENOTCONN
        assert payload.to_bytes() == b"payload"
