#include "../common/perf_multi_socket_reqrep.hpp"

namespace
{

perf_multi_socket_reqrep::endpoint_config_t make_config ()
{
    perf_multi_socket_reqrep::endpoint_config_t config;
    config.pattern_name = "MULTI_DEALER_ROUTER_REQREP";
    config.token = "dealer_router_reqrep";
    config.client_socket_type = ZLINK_SOCKET_DEALER;
    config.server_socket_type = ZLINK_SOCKET_ROUTER;
    config.client_router_request = false;
    config.server_has_routing_id = false;
    config.server_routing_id = "SERVER";
    return config;
}

} // namespace

int main (int argc, char **argv)
{
    if (argc < 3)
        return 1;

    const perf_multi_socket_reqrep::endpoint_config_t config = make_config ();
    if (!multi_perf_validate_recv_mode_for_pattern (config.pattern_name))
        return 1;

    const std::string lib_name = argv[1];
    const std::string transport = argv[2];
    return perf_multi_socket_reqrep::run_server_benchmark (config, lib_name, transport);
}
