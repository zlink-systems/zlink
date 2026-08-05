#include "../../../../perf/common/streamclient/perf_stream_bench_client.hpp"
#include "../../../../perf/common/streamclient/perf_stream_client_options.hpp"
#include "perf_common_multi.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

inline int parse_positive_env (const char *name_, int default_value_)
{
    if (!name_ || !*name_)
        return default_value_;

    const char *value = std::getenv (name_);
    if (!value || !*value)
        return default_value_;

    char *end = NULL;
    errno = 0;
    const long parsed = std::strtol (value, &end, 10);
    if (errno != 0 || end == value || parsed <= 0)
        return default_value_;
    if (parsed > INT_MAX)
        return INT_MAX;
    return static_cast<int> (parsed);
}

inline std::vector<size_t> parse_size_list_env (const char *name_)
{
    std::vector<size_t> sizes;
    if (!name_ || !*name_)
        return sizes;

    const char *value = std::getenv (name_);
    if (!value || !*value)
        return sizes;

    const char *cur = value;
    while (*cur) {
        while (*cur == ',' || *cur == ' ' || *cur == '\t')
            ++cur;
        if (!*cur)
            break;

        char *end = NULL;
        errno = 0;
        const unsigned long parsed = std::strtoul (cur, &end, 10);
        if (errno == 0 && end != cur && parsed > 0)
            sizes.push_back (static_cast<size_t> (parsed));

        if (!end || end == cur)
            break;
        cur = end;
    }

    return sizes;
}

inline bool parse_endpoint_arg (int argc_, char **argv_, std::string *endpoint_out_)
{
    if (!endpoint_out_)
        return false;

    endpoint_out_->clear ();
    for (int i = 4; i + 1 < argc_; ++i) {
        if (std::strcmp (argv_[i], "--endpoint") == 0) {
            *endpoint_out_ = argv_[i + 1];
            return !endpoint_out_->empty ();
        }
    }
    return false;
}

} // namespace

int main (int argc, char **argv)
{
    if (argc < 4)
        return 1;
    if (!multi_perf_validate_recv_mode_for_pattern ("MULTI_STREAM"))
        return 1;

    std::string endpoint;
    if (!parse_endpoint_arg (argc, argv, &endpoint)) {
        std::fprintf (stderr, "missing --endpoint\n");
        return 1;
    }

    client_options_t opt;
    opt.transport = argv[2];
    opt.pattern = "MULTI_STREAM";
    opt.send_stop_token = 0;
    opt.ccu = parse_positive_env ("BENCH_MULTI_CLIENTS", opt.ccu);
    opt.runs = 1;
    opt.duration = resolve_multi_int_env ("BENCH_MULTI_DURATION_SECONDS", 5, 1);
    opt.completion_wait_ms = resolve_multi_int_env ("BENCH_MULTI_DRAIN_MS", 300, 0);
    opt.size_transition_completion_wait_ms =
      resolve_multi_int_env ("BENCH_MULTI_PATTERN_TRANSITION_MS", 0, 0);
    opt.io_threads = parse_positive_env ("BENCH_IO_THREADS", opt.io_threads);

    std::vector<size_t> sizes = parse_size_list_env ("BENCH_MSG_SIZES");
    if (!sizes.empty ())
        opt.sizes.swap (sizes);

    std::string endpoint_transport;
    std::string endpoint_host;
    int endpoint_port = 0;
    if (!perf_stream_common::perf_stream_parse_endpoint (endpoint, &endpoint_transport,
                                                         &endpoint_host, &endpoint_port)) {
        std::fprintf (stderr, "invalid --endpoint: %s\n", endpoint.c_str ());
        return 1;
    }

    opt.transport = endpoint_transport;
    opt.host = endpoint_host;
    opt.port = endpoint_port;

    bench_client_t app (opt);
    return app.run ();
}
