/* SPDX-License-Identifier: Apache-2.0 */
#include "ZLinkStreamConnector.h"
#include <zlink/stream_connector.hpp>
#include "runtime/connector_runtime.hpp"
#include "control_frame_contract.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
// Verify the adapter queue boundary without an engine SDK.
#define private public
#include "../engines/unreal/Source/ZLinkStreamConnector/Private/ZLinkStreamConnector.cpp"
#undef private

int main ()
{
    using namespace zlink::stream_connector;
    using UE::ZLinkStreamConnector::Private::FZLinkStreamConnectorRuntime;
    UZLinkStreamConnector owner;
    FZLinkStreamConnectorRuntime runtime (owner);
    connector_options_t options;
    options.endpoint = "tcp://127.0.0.1:1";
    runtime.Connector = connector_factory_t::create (options);
    std::vector<int> order;
    const auto main_thread = std::this_thread::get_id ();
    bool main_thread_only = true;
    auto record = [&] (int value) {
        main_thread_only &= std::this_thread::get_id () == main_thread;
        order.push_back (value);
    };
    bool state_order = true;
    runtime.On ("push", [&] (const FZLinkStreamPacket &) {
        state_order &= owner.OnConnectionStateChanged.NumBroadcasts () == 0;
        record (1);
    });
    runtime.OnReplyReceived ([&] (const FZLinkStreamReplyReceivedContext &) {
        state_order &= owner.OnConnectionStateChanged.NumBroadcasts () == 1;
        record (3);
    });
    auto state = std::static_pointer_cast<detail::connector_state_t> (
      connector_internal_handle (runtime.Connector));
    std::thread receive ([&] {
        detail::connector_runtime_t::from (runtime.Connector).receive_packet (packet_t{"push"});
        detail::schedule_delivery (state, [&] {
            FZLinkStreamConnectorRuntime::EnqueueState (runtime.Pending,
                                                        EZLinkStreamConnectionState::Connected);
        });
        reply_received_context_t context;
        detail::schedule_reply_received (connector_internal_handle (runtime.Connector), context);
        runtime.RequestJson ("request", "{}", 1.0f,
                             [&] (const FZLinkStreamRequestResult &) { record (4); });
        while (runtime.Connector.pending_dispatch_count () > 0)
            runtime.Connector.dispatch ();
    });
    receive.join ();
    if (!order.empty ()) {
        std::cerr << "V19 delegate ran before main thread dispatch\n";
        return 1;
    }
    runtime.Dispatch ();
    const bool pass = order == std::vector<int> ({1, 3, 3, 4}) && main_thread_only && state_order;
    std::cout << "V19 FIFO packet/state/reply/request on main thread: " << (pass ? "PASS" : "FAIL")
              << '\n';
    for (const auto value : order)
        std::cout << value << ' ';
    std::cout << '\n';
    const auto control_failures =
      control_contract::connector_cases ([] (const connector_options_t &options, auto test) {
          UZLinkStreamConnector control_owner;
          FZLinkStreamConnectorRuntime control_runtime (control_owner);
          control_runtime.Connector = connector_factory_t::create (options);
          std::size_t delivered = 0;
          control_runtime.On ("$zlink.future", [&] (const FZLinkStreamPacket &) { ++delivered; });
          control_runtime.On ("$zlink.heartbeat.pong",
                              [&] (const FZLinkStreamPacket &) { ++delivered; });
          test (control_runtime.Connector);
          control_runtime.Dispatch ();
          if (delivered != 0)
              throw std::runtime_error ("Unreal delivered an ignored control");
      });
    return pass && control_failures == 0 ? 0 : 1;
}
