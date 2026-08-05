/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "Configuration/provider_options.hpp"
#include "provider_host_factory.hpp"

#include <zlink/framework.hpp>

namespace rl_provider = zlink::framework::e2e::resilience_lifecycle::provider;

int main (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    const auto options = rl_provider::read_provider_options (app, argc, argv);
    app.logging ()
      .use_file (options.log_dir + "/" + options.rid + ".log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &framework) {
        rl_provider::configure_provider_host (framework, options);
    });
    return app.run (argc, argv);
}
