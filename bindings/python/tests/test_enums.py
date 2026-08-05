import unittest

import zlink


class EnumValueTests(unittest.TestCase):
    def test_socket_type_values(self):
        self.assertEqual(int(zlink.SocketType.PAIR), 0x1001)
        self.assertEqual(int(zlink.SocketType.STREAM), 0x1008)

    def test_context_option_values(self):
        self.assertEqual(int(zlink.ContextOption.IO_THREADS), 1)
        self.assertEqual(int(zlink.ContextOption.MAX_SOCKETS), 2)
        self.assertEqual(int(zlink.ContextOption.CTX_OPT_BLOCKY), 10)
        self.assertEqual(int(zlink.ContextOption.AUTO_HWM_PROFILE), 17)
        self.assertEqual(int(zlink.ContextOption.AUTO_HWM_MSG_UNIT_BYTES), 18)
        self.assertEqual(int(zlink.AutoHwmProfile.COMPACT), 0)
        self.assertEqual(int(zlink.AutoHwmProfile.LOW_LATENCY), 1)
        self.assertEqual(int(zlink.AutoHwmProfile.BALANCED), 2)
        self.assertEqual(int(zlink.AutoHwmProfile.THROUGHPUT), 3)

    def test_send_and_recv_flag_values(self):
        self.assertEqual(int(zlink.SendFlags.NONE), 0)
        self.assertEqual(int(zlink.SendFlags.DONT_WAIT), 1)
        self.assertEqual(int(zlink.RecvFlags.NONE), 0)
        self.assertEqual(int(zlink.RecvFlags.DONT_WAIT), 1)

    def test_submit_result_values(self):
        self.assertEqual(int(zlink.SubmitResult.OK), 0)
        self.assertEqual(int(zlink.SubmitResult.BACKPRESSURED), 1)
        self.assertEqual(int(zlink.SubmitResult.INTERNAL_ERROR), 12)

    def test_request_recv_handler_close_bind_connect_config_results(self):
        self.assertEqual(int(zlink.RequestResult.OK), 0)
        self.assertEqual(int(zlink.RequestResult.PROTOCOL_ERROR), 104)
        self.assertEqual(int(zlink.RecvResult.NO_DATA), 201)
        self.assertEqual(int(zlink.HandlerResult.INVALID_HANDLE), 305)
        self.assertEqual(int(zlink.CloseResult.INVALID_HANDLE), 403)
        self.assertEqual(int(zlink.BindResult.ADDR_IN_USE), 502)
        self.assertEqual(int(zlink.ConnectResult.NOT_SUPPORTED), 602)
        self.assertEqual(int(zlink.ConfigResult.NOT_SUPPORTED), 703)

    def test_result_enums_match_core_11_contract(self):
        expected = {
            zlink.RequestResult: {
                0, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113
            },
            zlink.RecvResult: {0, 201, 202, 203, 204, 205, 206, 207, 208},
            zlink.ConnectResult: {0, 601, 602, 603, 604, 605, 606, 607, 608},
            zlink.ConfigResult: {0, 701, 702, 703, 704, 705, 706, 707, 708, 709},
        }
        for enum_type, values in expected.items():
            self.assertEqual({int(value) for value in enum_type}, values)

    def test_unsupported_context_option_is_not_public(self):
        with zlink.create_context() as context:
            self.assertFalse(hasattr(context.options, "thread_priority"))

    def test_monitor_event_values(self):
        self.assertEqual(int(zlink.MonitorEventMask.CONNECTED), 0x0001)
        self.assertEqual(int(zlink.MonitorEventMask.ALL), 0xFFFF)

    def test_error_code_values(self):
        self.assertEqual(int(zlink.ErrorCode.EFSM), 156384763)
        self.assertEqual(int(zlink.ErrorCode.EMTHREAD), 156384766)

class EnumTypeTests(unittest.TestCase):
    def test_int_enum_is_int(self):
        self.assertIsInstance(zlink.SocketType.PAIR, int)
        self.assertIsInstance(zlink.SendFlags.DONT_WAIT, int)
        self.assertIsInstance(zlink.SubmitResult.OK, int)

    def test_int_flag_is_int(self):
        self.assertIsInstance(zlink.MonitorEventMask.CONNECTED, int)
        self.assertIsInstance(zlink.PollEventFlag.POLLIN, int)

    def test_flag_or_operation(self):
        combined = zlink.MonitorEventMask.CONNECTED | zlink.MonitorEventMask.DISCONNECTED
        self.assertEqual(int(combined), 0x0201)


if __name__ == "__main__":
    unittest.main()
