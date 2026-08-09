#include "../common/perf_multi_reqrep.hpp"

int main (int argc, char **argv)
{
    if (argc < 4)
        return 1;
    const perf::multi::reqrep::config_t config = {
      "ROUTER_ROUTER_REQREP", "MULTI_ROUTER_ROUTER_REQREP", true, true};
    return perf::multi::reqrep::run_server (
             config, argv[1], argv[2], std::strtoull (argv[3], NULL, 10))
             ? 0
             : 1;
}
