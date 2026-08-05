/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

namespace zlink::framework::e2e::spot_actor_transfer::server
{

enum class host_role_t
{
    actor_node,
    session
};

int run_host (host_role_t role, int argc, char **argv);

} // namespace zlink::framework::e2e::spot_actor_transfer::server
