/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/mesh/route_mesh_runtime_service.hpp"

#include <cassert>
#include <chrono>

namespace zlink::framework::runtime
{
struct route_mesh_runtime_service_test_access_t
{
    static void verify_management_wait ()
    {
        using clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;
        auto now = clock::time_point (10s);
        int refreshed = 0;
        int waits = 0;
        route_mesh_runtime_service_t::pump_management (
          [&] { return now; },
          [&] {
              ++refreshed;
              now += 7s; // Store work may finish much later than its deadline.
          },
          [&] (clock::time_point due) {
              ++waits;
              assert (due == now + 100ms);
              if (waits == 2)
                  return false; // Shutdown does not run another Store query.
              now = due;
              return true;
          });
        assert (waits == 2 && refreshed == 1);
    }
};
} // namespace zlink::framework::runtime

int main ()
{
    zlink::framework::runtime::route_mesh_runtime_service_test_access_t::verify_management_wait ();
}
