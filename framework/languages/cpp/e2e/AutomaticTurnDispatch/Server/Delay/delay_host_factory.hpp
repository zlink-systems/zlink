/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Shared/codecs.hpp"
#include "Handlers/delay_handler.hpp"
#include "Support/delay_support.hpp"

#include <zlink/framework.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::delay {

namespace yd = zlink::framework::e2e::automatic_turn_dispatch;

inline std::uint16_t delay_port_from_endpoint (const std::string &endpoint)
{
    const auto separator = endpoint.rfind (':');
    if (separator == std::string::npos || separator + 1 >= endpoint.size ())
        throw std::invalid_argument ("delay endpoint must include a port");
    const auto value = std::stoul (endpoint.substr (separator + 1));
    if (value == 0 || value > 65535)
        throw std::invalid_argument ("delay endpoint port is out of range");
    return static_cast<std::uint16_t> (value);
}

inline void configure_delay_host (zlink::framework::app_t &app,
                                  const delay_options_t &delay_options)
{
    app.logging ()
      .use_file (delay_options.log_dir + "/" + delay_options.node_rid + ".log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([=] (zlink::framework::zlink_framework_options_t &options) {
        options.configure_dispatch ()
          .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
          .trace_log_file (delay_options.log_dir + "/" + delay_options.node_rid + "-flow.log")
          .trace_label ("cpp-atd-" + delay_options.node_rid);
        auto state = std::make_unique<delay_state_t> (
          delay_options.node_rid,
          delay_options.log_dir + "/" + delay_options.node_rid + ".evidence.log");
        options.services ().add_singleton<delay_state_t> (std::move (state));
        server::configure_codecs (options.codecs ());
        options.add_client_server_channel (yd::delay_channel)
          .server ()
          .listen (delay_port_from_endpoint (delay_options.delay_endpoint))
          .add_handler_group (yd::handler_group);
        options.handlers ().group (yd::handler_group).add<delay_handler_t> ();
        options.http ().listen (delay_options.http_endpoint).map_health ("/health");
    });
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::delay
