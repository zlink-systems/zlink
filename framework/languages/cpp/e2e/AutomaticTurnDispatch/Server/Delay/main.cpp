/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "delay_host_factory.hpp"
#include "../Shared/configuration.hpp"

int main (int argc, char **argv)
{
    namespace atd = zlink::framework::e2e::automatic_turn_dispatch::server;
    auto app = zlink::framework::app_t::create ();
    const auto options = atd::read_role_options<atd::delay::delay_options_t> (
      app, argc, argv, "delay");
    atd::delay::configure_delay_host (app, options);
    return app.run (argc, argv);
}
