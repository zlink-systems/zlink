/* SPDX-License-Identifier: MPL-2.0 */

#include "../Shared/observability_host.hpp"

int main (int argc, char **argv)
{
    using namespace zlink::framework::e2e::observability_ops::server;
    return run_host (host_role_t::session, argc, argv);
}
