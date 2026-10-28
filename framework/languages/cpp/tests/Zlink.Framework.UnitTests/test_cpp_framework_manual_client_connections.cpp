/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/channels/channel_runtime_bundle.hpp"

#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

int main ()
{
    using zlink::framework::detail::channel_runtime_bundle_t;
    channel_runtime_bundle_t bundle;
    assert (!bundle.has_manual_connections ());
    bundle.try_add_manual_connection ("tcp://first:7400");
    std::vector<std::string> calls;
    bool fail_connect = false;
    bool fail_disconnect = false;
    bundle.attach_manual_connections (
      [&] (const std::string &endpoint) {
          if (fail_connect)
              throw std::runtime_error ("connect failure");
          calls.push_back ("connect " + endpoint);
      },
      [&] (const std::string &endpoint) {
          if (fail_disconnect)
              throw std::runtime_error ("disconnect failure");
          calls.push_back ("disconnect " + endpoint);
      });
    assert (calls == std::vector<std::string>{"connect tcp://first:7400"});
    for (int i = 0; i < 1000; ++i) {
        assert (bundle.has_manual_connections ());
    }
    assert (calls.size () == 1);
    assert (!bundle.try_add_manual_connection ("tcp://first:7400"));
    assert (calls.size () == 1);
    assert (bundle.try_add_manual_connection ("tcp://second:7400"));
    assert (calls.back () == "connect tcp://second:7400");
    fail_connect = true;
    bool caught = false;
    try {
        bundle.try_add_manual_connection ("tcp://failed:7400");
    }
    catch (const std::runtime_error &) {
        caught = true;
    }
    assert (caught);
    assert (!bundle.contains_manual_connection ("tcp://failed:7400"));
    fail_connect = false;
    fail_disconnect = true;
    caught = false;
    try {
        bundle.remove_manual_connection ("tcp://first:7400");
    }
    catch (const std::runtime_error &) {
        caught = true;
    }
    assert (caught);
    assert (bundle.contains_manual_connection ("tcp://first:7400"));
    fail_disconnect = false;
    bundle.remove_manual_connection ("tcp://first:7400");
    assert (calls.back () == "disconnect tcp://first:7400");
    const auto count = calls.size ();
    bundle.remove_manual_connection ("tcp://first:7400");
    assert (calls.size () == count);
    bundle.remove_manual_connection ("tcp://second:7400");
    assert (!bundle.has_manual_connections ());
    return 0;
}
