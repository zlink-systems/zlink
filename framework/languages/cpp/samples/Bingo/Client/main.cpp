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
    [[maybe_unused]] auto inbound_log1 = core_client1.observe_inbound (
      [] (const zlink::stream_connector::inbound_observation_t &observation) {
          std::cout << "stream-inbound sample=Bingo client=player1 kind="
                    << static_cast<int> (observation.kind) << " name=" << observation.name
                    << " seq="
                    << (observation.request_seq ? std::to_string (*observation.request_seq)
                                                : std::string ("-"))
                    << " bytes=" << observation.payload_length << '\n';
      });
    [[maybe_unused]] auto inbound_log2 = core_client2.observe_inbound (
      [] (const zlink::stream_connector::inbound_observation_t &observation) {
          std::cout << "stream-inbound sample=Bingo client=player2 kind="
                    << static_cast<int> (observation.kind) << " name=" << observation.name
                    << " seq="
                    << (observation.request_seq ? std::to_string (*observation.request_seq)
                                                : std::string ("-"))
                    << " bytes=" << observation.payload_length << '\n';
      });
    [[maybe_unused]] auto inbound_log3 = core_observer.observe_inbound (
      [] (const zlink::stream_connector::inbound_observation_t &observation) {
          std::cout << "stream-inbound sample=Bingo client=observer kind="
                    << static_cast<int> (observation.kind) << " name=" << observation.name
                    << " seq="
                    << (observation.request_seq ? std::to_string (*observation.request_seq)
                                                : std::string ("-"))
                    << " bytes=" << observation.payload_length << '\n';
      });

    auto client1 = zlink::stream_e2e_client::use (core_client1);
    auto client2 = zlink::stream_e2e_client::use (core_client2);
    auto observer = zlink::stream_e2e_client::use (core_observer);
    const auto completed = bingo_client_scenario_t{}.run (client1, client2, observer);
    if (completed) {
        inbound_log1.close ();
        inbound_log2.close ();
        inbound_log3.close ();
        std::cout << "bingo=completed" << std::endl;
    }
    return completed ? 0 : 1;
}
