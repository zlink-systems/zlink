# SPDX-License-Identifier: MPL-2.0

from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack

import pytest
import zlink


MAX_TIMEOUT_MS = 2**32 - 1


@pytest.mark.parametrize("timeout", [0, MAX_TIMEOUT_MS / 1000])
def test_request_accepts_zero_and_max_uint32_millisecond_timeout(timeout):
    with (
        zlink.create_context() as context,
        zlink.create_dealer_socket(context) as dealer,
        zlink.create_router_socket(context) as router,
        zlink.create_received() as request,
        ThreadPoolExecutor(1) as executor,
    ):
        endpoint = "inproc://request-timeout-boundary"
        router.options.receive_timeout_ms = 1000
        router.bind(endpoint)
        dealer.connect(endpoint)
        operation = dealer.request().message(b"request").timeout(timeout)
        pending = executor.submit(operation.submit_sync)
        try:
            assert router.recv_into(request)
            request.reply().message(b"reply").submit()
            with ExitStack() as replies:
                parts = [replies.enter_context(part) for part in pending.result(timeout=2)]
                assert [part.to_bytes() for part in parts] == [b"reply"]
        finally:
            if not pending.done():
                context.shutdown()


@pytest.mark.parametrize("timeout", [-0.001, (MAX_TIMEOUT_MS + 1) / 1000, float("nan"), float("inf")])
def test_request_rejects_timeout_outside_native_range(timeout):
    with zlink.create_context() as context, zlink.create_dealer_socket(context) as dealer:
        dealer.options.send_timeout_ms = 0
        with pytest.raises(ValueError, match="timeout"):
            dealer.request().message(b"request").timeout(timeout).submit_sync()
