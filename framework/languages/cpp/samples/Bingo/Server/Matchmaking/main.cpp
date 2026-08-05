/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "matchmaking_server_host_factory.hpp"

using namespace zlink;

int main (int argc, char **argv)
{
    using namespace zlink::samples::bingo;
    using namespace framework;

    auto app = app_t::create ();
    load_sample_configuration (app, argc, argv);
    const auto topology = sample_topology_from_config (app);
    matchmaking_server_host_factory_t::configure (
      app, topology, !sample_keep_running (app));
    return app.run (argc, argv);
}
