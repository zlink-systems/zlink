import threading

from zlink._runtime.eventing.dispatcher import CallbackDispatcher


def test_dispatcher_drains_callbacks_and_rejects_submissions_after_close():
    entered = []
    left = []
    completed = threading.Event()
    dispatcher = CallbackDispatcher(
        "test-dispatcher",
        lambda: entered.append(True),
        lambda: left.append(True),
    )

    assert dispatcher.submit(completed.set)
    assert completed.wait(1.0)

    dispatcher.close()
    assert dispatcher.submit(lambda: None) is False
    dispatcher.close()
    assert entered == [True]
    assert left == [True]
    assert dispatcher.closed
