// perf_stream_client entrypoint.
// Parses CLI options and runs the async multi-connection benchmark.
// Optionally sends a stop token to the echo server after the benchmark.

#include "perf_stream_bench_client.hpp"
#include "perf_stream_client_options.hpp"
#include "../../multi/common/perf_common_multi.hpp"

#include <cstdio>

// Parse options, run benchmark, and send stop token if requested.
static int perf_stream_client_run (int argc, char **argv)
{
    if (argc <= 1) {
        std::printf ("perf_stream_client: no args -> skip\n");
        return 0;
    }

    client_options_t opt;
    if (!parse_options (argc, argv, opt))
        return 2;

    bench_client_t app (opt);
    int rc = app.run ();

    if (opt.send_stop_token > 0 && !send_stop_token_once (opt))
        rc = 2;
    return rc;
}

int main (int argc, char **argv)
{
    if (!multi_perf_validate_recv_mode_for_pattern ("MULTI_STREAM"))
        return 1;
    return perf_stream_client_run (argc, argv);
}
