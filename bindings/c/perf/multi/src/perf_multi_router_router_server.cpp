#include "../common/perf_multi_relay_server.hpp"

namespace
{

static const char *k_pattern = "MULTI_ROUTER_ROUTER_SENDSEND";
static const char *k_token = "router_router";
static const zlink_socket_type_t k_server_socket_type = ZLINK_SOCKET_ROUTER;
static const bool k_server_has_routing_id = true;
static const char *k_server_routing_id = "SERVER";

} // namespace

int main (int argc, char **argv)
{
    if (argc < 3)
        return 1;
    if (!multi_perf_validate_recv_mode_for_pattern (k_pattern))
        return 1;

    perf_multi_relay_server::relay_server_config_t config;
    config.pattern_name = k_pattern;
    config.token = k_token;
    config.socket_type = k_server_socket_type;
    config.has_server_routing_id = k_server_has_routing_id;
    config.server_routing_id = k_server_routing_id;
    config.msg_size = argc >= 4 ? static_cast<size_t> (std::strtoull (argv[3], NULL, 10)) : 0;
    if (argc >= 4 && config.msg_size == 0)
        return 1;

    const std::string lib_name = argv[1];
    const std::string transport = argv[2];
    return perf_multi_relay_server::run_server_benchmark (config, lib_name, transport);
}
