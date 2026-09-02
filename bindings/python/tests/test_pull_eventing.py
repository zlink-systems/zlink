import zlink


def test_monitor_and_timer_expose_pull_only_surface():
    assert {"status", "recv", "close"}.issubset(dir(zlink.MonitorSocket))
    assert {"start", "stop", "recv", "close"}.issubset(dir(zlink.Timer))

    with zlink.create_timer() as timer:
        assert timer.recv() is None


def test_monitor_no_data_is_none():
    with zlink.create_context() as context:
        with zlink.create_pair_socket(context) as socket:
            with socket.monitor_open() as monitor:
                assert monitor.recv(flags=zlink.RecvFlags.DONT_WAIT) is None
