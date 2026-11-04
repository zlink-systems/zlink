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
#include "../engines/unreal/Source/ZLinkStreamConnector/Private/ZLinkStreamConnector.cpp"

namespace UE::ZLinkStreamConnector::Private
{
struct unreal_stream_connector_test_access_t
{
    static auto &connector (FZLinkStreamConnectorRuntime &runtime) { return runtime.Connector; }

    static void enqueue_connected (FZLinkStreamConnectorRuntime &runtime)
    {
        FZLinkStreamConnectorRuntime::EnqueueState (runtime.Pending,
                                                    EZLinkStreamConnectionState::Connected);
    }
};
} // namespace UE::ZLinkStreamConnector::Private

int main ()
{
    using namespace zlink::stream_connector;
    using UE::ZLinkStreamConnector::Private::FZLinkStreamConnectorRuntime;
    using UE::ZLinkStreamConnector::Private::unreal_stream_connector_test_access_t;
    UZLinkStreamConnector owner;
    FZLinkStreamConnectorRuntime runtime (owner);
    connector_options_t options;
    options.endpoint = "tcp://127.0.0.1:1";
    auto &connector = unreal_stream_connector_test_access_t::connector (runtime);
    connector = connector_factory_t::create (options);
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
    auto state =
      std::static_pointer_cast<detail::connector_state_t> (connector_internal_handle (connector));
    std::thread receive ([&] {
        detail::connector_runtime_t::from (connector).receive_packet (packet_t{"push"});
        detail::schedule_delivery (
          state, [&] { unreal_stream_connector_test_access_t::enqueue_connected (runtime); });
        reply_received_context_t context;
        detail::schedule_reply_received (connector_internal_handle (connector), context);
        runtime.RequestJson ("request", "{}", 1.0f,
                             [&] (const FZLinkStreamRequestResult &) { record (4); });
        while (connector.pending_dispatch_count () > 0)
            connector.dispatch ();
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
          unreal_stream_connector_test_access_t::connector (control_runtime) =
            connector_factory_t::create (options);
          std::size_t delivered = 0;
          control_runtime.On ("$zlink.future", [&] (const FZLinkStreamPacket &) { ++delivered; });
          control_runtime.On ("$zlink.heartbeat.pong",
                              [&] (const FZLinkStreamPacket &) { ++delivered; });
          test (unreal_stream_connector_test_access_t::connector (control_runtime));
          control_runtime.Dispatch ();
          if (delivered != 0)
              throw std::runtime_error ("Unreal delivered an ignored control");
      });
    return pass && control_failures == 0 ? 0 : 1;
}
