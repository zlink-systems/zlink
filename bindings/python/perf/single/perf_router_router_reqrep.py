import sys

from perf_single_reqrep import run_reqrep_pattern


if __name__ == "__main__":
    run_reqrep_pattern(
        sys.argv[1:],
        pattern="ROUTER_ROUTER_REQREP",
        routed_request=True,
    )
