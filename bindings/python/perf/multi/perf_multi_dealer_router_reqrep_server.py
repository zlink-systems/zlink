import sys

from perf_multi_reqrep_server import run_reqrep_server


if __name__ == "__main__":
    run_reqrep_server(
        sys.argv[1:],
        endpoint_token="multi-dealer-router-reqrep",
        routed_server=False,
    )
