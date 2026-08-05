/* SPDX-License-Identifier: MPL-2.0 */

#include "../Shared/node_host.hpp"

int main (int argc, char **argv)
{
    using namespace zlink::framework::e2e::spot_actor_transfer::server;
    return run_host (host_role_t::actor_node, argc, argv);
}
