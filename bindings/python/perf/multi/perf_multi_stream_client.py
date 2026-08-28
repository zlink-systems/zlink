"""Disabled entrypoint for the non-measured Python STREAM client role.

MULTI_STREAM always uses the shared C perf_stream_client as its external peer;
the measured binding surface is the Python STREAM server packet handler.
"""


def main(argv=None):
    del argv
    raise SystemExit(
        "perf_multi_stream_client.py is disabled: the MULTI_STREAM client "
        "must be the shared C perf_stream_client binary "
        "(bindings/c/perf/common/streamclient). run_benchmarks.py spawns it "
        "automatically."
    )


if __name__ == "__main__":
    main()
