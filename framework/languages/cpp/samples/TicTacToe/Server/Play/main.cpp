/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "play_server_host_factory.hpp"

using namespace zlink;

int main (int argc, char **argv)
{
    using namespace zlink::samples::tictactoe;
    using namespace framework;

    auto app = app_t::create ();
    load_sample_configuration (app, argc, argv);
    const auto topology = sample_topology_from_config (app);
    play_server_host_factory_t::configure (app, topology);
    if (!sample_keep_running (app)) {
        app.add_hosted_service (std::make_unique<stop_after_start_service_t> (app));
    }
    return app.run (argc, argv);
}
