/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "session_host_factory.hpp"
#include "../Shared/configuration.hpp"

int main (int argc, char **argv)
{
    namespace atd = zlink::framework::e2e::automatic_turn_dispatch::server;
    auto app = zlink::framework::app_t::create ();
    const auto options = atd::read_role_options<atd::session::session_options_t> (
      app, argc, argv, "session");
    atd::session::configure_session_host (app, options);
    return app.run (argc, argv);
}
