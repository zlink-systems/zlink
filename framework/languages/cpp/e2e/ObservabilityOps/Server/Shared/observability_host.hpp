/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

namespace zlink::framework::e2e::observability_ops::server
{

enum class host_role_t
{
    session,
    play,
    order_workflow
};

int run_host (host_role_t role, int argc, char **argv);

} // namespace zlink::framework::e2e::observability_ops::server
