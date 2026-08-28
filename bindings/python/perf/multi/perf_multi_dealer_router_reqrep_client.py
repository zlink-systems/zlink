import asyncio
import sys

from perf_multi_reqrep_client import run_reqrep_client


if __name__ == "__main__":
    asyncio.run(
        run_reqrep_client(
            sys.argv[1:],
            pattern="MULTI_DEALER_ROUTER_REQREP",
            routed_request=False,
        )
    )
