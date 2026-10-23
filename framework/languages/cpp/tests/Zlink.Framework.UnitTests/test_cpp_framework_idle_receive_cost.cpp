/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework.hpp>
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace fw = zlink::framework;
using dispatch_t = fw::runtime::mesh::service_mailbox_t::application_dispatch_t;
using deadline_t = std::optional<std::chrono::steady_clock::time_point>;
// GNU ld wrappers count calls made by the real hosted receive role.
// They preserve each result and call the original implementation.
namespace
{
std::mutex mutex;
std::condition_variable changed;
std::size_t empty_receives = 0, empty_handoffs = 0, empty_waits = 0;
enum class boundary_t
{
    none,
    handoff,
    wait
};
boundary_t first_boundary = boundary_t::none;
thread_local bool last_empty = false;
void observed (std::size_t &counter, boundary_t boundary = boundary_t::none)
{
    {
        std::lock_guard lock (mutex);
        ++counter;
        if (first_boundary == boundary_t::none)
            first_boundary = boundary;
    }
    changed.notify_one ();
}
}
extern "C" fw::task_t<std::size_t>
real_dispatch (fw::detail::mesh_node_runtime_t *self,
               dispatch_t callback,
               bool accept,
               std::function<bool ()>
                 next) __asm__ ("__real__ZN5zlink9framework6detail19mesh_node_runtime_t14dispatch_"
                                "readyESt8functionIFvRKNS0_7runtime4host14ready_record_tERKNS5_"
                                "16receive_record_tESt6vectorINS_9message_tESaISD_EEEEbS3_IFbvEE");
extern "C" fw::task_t<std::size_t>
wrap_dispatch (fw::detail::mesh_node_runtime_t *self,
               dispatch_t callback,
               bool accept,
               std::function<bool ()>
                 next) __asm__ ("__wrap__ZN5zlink9framework6detail19mesh_node_runtime_t14dispatch_"
                                "readyESt8functionIFvRKNS0_7runtime4host14ready_record_tERKNS5_"
                                "16receive_record_tESt6vectorINS_9message_tESaISD_EEEEbS3_IFbvEE");
extern "C" bool real_handoff (
  const std::function<bool ()> &handoff,
  const fw::detail::completion_dispatcher_borrow_t
    &dispatcher) __asm__ ("__real__ZN5zlink9framework6detail35dispatch_completion_receive_"
                          "handoffERKSt8functionIFbvEERKPNS1_23completion_dispatcher_tE");
extern "C" bool wrap_handoff (
  const std::function<bool ()> &handoff,
  const fw::detail::completion_dispatcher_borrow_t
    &dispatcher) __asm__ ("__wrap__ZN5zlink9framework6detail35dispatch_completion_receive_"
                          "handoffERKSt8functionIFbvEERKPNS1_23completion_dispatcher_tE");
extern "C" bool real_wait (
  fw::runtime::host::public_host_runtime_t *self,
  std::chrono::milliseconds timeout,
  bool accept,
  deadline_t
    next) __asm__ ("__real__ZN5zlink9framework7runtime4host21public_host_runtime_t26wait_for_"
                   "dispatch_activityENSt6chrono8durationIlSt5ratioILl1ELl1000EEEEbSt8optionalINS4_"
                   "10time_pointINS4_3_V212steady_clockENS5_IlS6_ILl1ELl1000000000EEEEEEE");
extern "C" bool wrap_wait (
  fw::runtime::host::public_host_runtime_t *self,
  std::chrono::milliseconds timeout,
  bool accept,
  deadline_t
    next) __asm__ ("__wrap__ZN5zlink9framework7runtime4host21public_host_runtime_t26wait_for_"
                   "dispatch_activityENSt6chrono8durationIlSt5ratioILl1ELl1000EEEEbSt8optionalINS4_"
                   "10time_pointINS4_3_V212steady_clockENS5_IlS6_ILl1ELl1000000000EEEEEEE");

extern "C" fw::task_t<std::size_t> wrap_dispatch (fw::detail::mesh_node_runtime_t *self,
                                                  dispatch_t callback,
                                                  bool accept,
                                                  std::function<bool ()> next)
{
    auto result = real_dispatch (self, std::move (callback), accept, std::move (next));
    last_empty = result.await_ready () && result.result ().value () == 0;
    if (last_empty)
        observed (empty_receives);
    return result;
}
extern "C" bool wrap_handoff (const std::function<bool ()> &handoff,
                              const fw::detail::completion_dispatcher_borrow_t &dispatcher)
{
    if (last_empty)
        observed (empty_handoffs, boundary_t::handoff);
    return real_handoff (handoff, dispatcher);
}
extern "C" bool wrap_wait (fw::runtime::host::public_host_runtime_t *self,
                           std::chrono::milliseconds timeout,
                           bool accept,
                           deadline_t next)
{
    if (last_empty)
        observed (empty_waits, boundary_t::wait);
    return real_wait (self, timeout, accept, next);
}
int main ()
{
    auto app = fw::app_t::create ();
    app.logging ().disable_record_capture ();
    app.add_zlink_framework ()
      .add_route_mesh ("idle-receive")
      .set_object_role (fw::object_role_t::none)
      .listen ("tcp://127.0.0.1:6589")
      .set_routing_id (zlink::routing_id_t::from ("idle-receive"));
    std::thread host ([&] { app.run (0, nullptr); });
    bool valid = false;
    {
        std::unique_lock lock (mutex);
        changed.wait (lock, [] { return empty_handoffs != 0 || empty_waits != 0; });
        valid = empty_receives != 0 && empty_waits != 0 && first_boundary == boundary_t::wait;
        std::cout << "first_boundary=" << static_cast<unsigned> (first_boundary)
                  << " empty_receives=" << empty_receives << " empty_handoffs=" << empty_handoffs
                  << " empty_waits=" << empty_waits << '\n';
    }
    app.request_stop ();
    host.join ();
    std::cout << "empty_receives=" << empty_receives << " empty_handoffs=" << empty_handoffs
              << " empty_waits=" << empty_waits << '\n';
    return valid ? 0 : 1;
}
