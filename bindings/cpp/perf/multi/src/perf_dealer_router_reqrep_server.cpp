#include "../common/perf_multi_reqrep.hpp"

int main (int argc, char **argv)
{
    if (argc < 4)
        return 1;
    const perf::multi::reqrep::config_t config = {
      "DEALER_ROUTER_REQREP", "MULTI_DEALER_ROUTER_REQREP", false};
    return perf::multi::reqrep::run_server (
             config, argv[1], argv[2], std::strtoull (argv[3], NULL, 10))
             ? 0
             : 1;
}
