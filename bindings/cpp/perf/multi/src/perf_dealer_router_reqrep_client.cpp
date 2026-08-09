#include "../common/perf_entry.hpp"
#include "../common/perf_multi_reqrep.hpp"

int main (int argc, char **argv)
{
    if (argc < 4)
        return 1;
    const std::string endpoint = perf::multi::parse_endpoint_arg (argc, argv);
    const perf::multi::reqrep::config_t config = {
      "DEALER_ROUTER_REQREP", "MULTI_DEALER_ROUTER_REQREP", false, false};
    return !endpoint.empty ()
               && perf::multi::reqrep::run_client<zlink::dealer_socket_t> (
                 config, argv[1], argv[2], std::strtoull (argv[3], NULL, 10), endpoint)
             ? 0
             : 1;
}
