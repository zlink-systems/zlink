/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "Configuration/subscriber_options.hpp"
#include "Endpoints/operational_endpoints.hpp"
#include "../Shared/location_store.hpp"

#include <memory>

namespace ps_subscriber = zlink::framework::e2e::pubsub::server::subscriber;
namespace ps_server = zlink::framework::e2e::pubsub::server;

int main (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    app.config ().load_cli (argc, argv);
    const auto config_path = app.config ().model ().get ("config");
    if (!config_path) {
        throw std::runtime_error ("PubSub subscriber requires --config=<path>");
    }
    app.config ().load_json (*config_path);
    const auto pubsub =
      app.config ().bind_required<ps_subscriber::subscriber_options_t> ("e2e");
    auto state =
      std::make_unique<ps_subscriber::evidence_store_t> (pubsub.subscriber_id,
                                                         pubsub.accepted_topics,
                                                         pubsub.handler_delay_ms);
    auto *state_ptr = state.get ();
    app.logging ()
      .use_file (pubsub.log_dir + "/" + pubsub.subscriber_id + ".log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.configure_dispatch ()
          .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
          .trace_log_file (pubsub.log_dir + "/" + pubsub.subscriber_id + "-flow.log")
          .trace_label ("cpp-ps-" + pubsub.subscriber_id)
          .set_message_flow_observer (
            [state_ptr] (const zlink::framework::message_flow_event_t &error) {
                state_ptr->record_error (error);
            });
        options.services ().add_singleton<ps_subscriber::evidence_store_t> (std::move (state));
        ps_server::configure_codecs (options.codecs ());
        ps_server::add_redis_location_store (options, pubsub.redis_endpoint, pubsub.redis_key_prefix);
        auto channel = options.add_fanout_channel (zlink::framework::e2e::pubsub::event_channel);
        channel.enable_subscriber ();
        channel.use_handler_group (zlink::framework::e2e::pubsub::handler_group);
        options.http ()
          .listen (pubsub.http_endpoint)
          .map_health ("/health")
          .map_get<ps_subscriber::evidence_handler_t> ("/evidence")
          .map_post<ps_subscriber::evidence_wait_handler_t> ("/evidence/wait");
        if (ps_server::has_topic (pubsub.topics, zlink::framework::e2e::pubsub::topic_fanout)) {
            options.handlers ()
              .group (zlink::framework::e2e::pubsub::handler_group)
              .add_publish<ps_subscriber::fanout_handler_t> ();
        }
        if (ps_server::has_topic (pubsub.topics, zlink::framework::e2e::pubsub::topic_alpha)) {
            options.handlers ()
              .group (zlink::framework::e2e::pubsub::handler_group)
              .add_publish<ps_subscriber::alpha_handler_t> ();
        }
        if (ps_server::has_topic (pubsub.topics, zlink::framework::e2e::pubsub::topic_beta)) {
            options.handlers ()
              .group (zlink::framework::e2e::pubsub::handler_group)
              .add_publish<ps_subscriber::beta_handler_t> ();
        }
    });
    return app.run (argc, argv);
}
