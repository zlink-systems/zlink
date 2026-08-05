/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "Configuration/consumer_options.hpp"
#include "consumer_host_factory.hpp"

#include <zlink/framework.hpp>

namespace rl_consumer = zlink::framework::e2e::resilience_lifecycle::consumer;

int main (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    const auto options = rl_consumer::read_consumer_options (app, argc, argv);
    app.logging ()
      .use_file (options.log_dir + "/consumer.log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &framework) {
        rl_consumer::configure_consumer_host (framework, options);
    });
    return app.run (argc, argv);
}
