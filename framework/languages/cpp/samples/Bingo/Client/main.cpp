/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "bingo_client_scenario.hpp"

#include "Configuration/sample_configuration.hpp"

#include <zlink/stream_connector.hpp>
#include <zlink/stream_e2e_client.hpp>
#include <zlink/codecs/protobuf.hpp>

#include <iostream>
#include <string>

int main (int argc, char **argv)
{
    using namespace zlink;
    using namespace zlink::samples::bingo;

    bingo_client_options_t options{load_sample_topology (argc, argv)};
    zlink::stream_connector::connector_options_t connector_options;
    connector_options.connect_timeout = options.connect_timeout;
    connector_options.request_timeout = options.request_timeout;
    connector_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;

    connector_options.endpoint = options.session_a_stream_endpoint;
    auto core_client1 = zlink::stream_connector::connector_factory_t::create (connector_options);
    core_client1.codecs ().use (zlink::framework_codecs::protobuf ());
    connector_options.endpoint = options.session_b_stream_endpoint;
    auto core_client2 = zlink::stream_connector::connector_factory_t::create (connector_options);
    core_client2.codecs ().use (zlink::framework_codecs::protobuf ());
    auto core_observer = zlink::stream_connector::connector_factory_t::create (connector_options);
    core_observer.codecs ().use (zlink::framework_codecs::protobuf ());

    auto client1 = zlink::stream_e2e_client::use (core_client1);
    auto client2 = zlink::stream_e2e_client::use (core_client2);
    auto observer = zlink::stream_e2e_client::use (core_observer);
    const auto completed = bingo_client_scenario_t{}.run (client1, client2, observer);
    if (completed) {
        std::cout << "bingo=completed" << std::endl;
    }
    return completed ? 0 : 1;
}
