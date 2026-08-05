/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"
#include <zlink.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

#include <future>
#include <thread>

namespace
{

static_assert (!std::is_constructible<zlink::socket_monitor_t, void *>::value,
               "socket_monitor_t must not expose a raw void* constructor");
static_assert (!std::is_constructible<zlink::socket_monitor_t,
                                      const zlink::socket_t &,
                                      zlink::monitor_event>::value,
               "socket_monitor_t must not expose a public direct constructor");

template <typename T> class has_open_t
{
  private:
    template <typename U>
    static auto test (int) -> decltype (U::open (std::declval<const zlink::socket_t &> (),
                                                 zlink::monitor_event::all),
                                        std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

static_assert (has_open_t<zlink::socket_monitor_t>::value, "socket_monitor_t must expose open");
static_assert (
  std::is_same<decltype (zlink::monitor_status_t ().auto_hwm_applied_sndhwm_bytes),
               uint64_t>::value,
  "monitor applied send HWM must be a uint64_t byte value");
static_assert (
  std::is_same<decltype (zlink::monitor_status_t ().snd_bytes_in_flight), uint64_t>::value,
  "monitor in-flight bytes must be uint64_t");

template <typename T> class has_on_event_t
{
  private:
    template <typename U>
    static auto test (int) -> decltype (std::declval<U &> ().on_event (
                                          std::function<void (const zlink::monitor_event_t &)> ()),
                                        std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

static_assert (has_on_event_t<zlink::socket_monitor_t>::value,
               "socket_monitor_t must expose on_event");

template <typename T> class has_ignore_event_t
{
  private:
    template <typename U>
    static auto
    test (int) -> decltype (U::ignore_event (std::declval<const zlink::monitor_event_t &> ()),
                            std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

template <typename T> class has_size_t
{
  private:
    template <typename U>
    static auto test (int) -> decltype (std::declval<const U &> ().size (), std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

static_assert (has_ignore_event_t<zlink::socket_monitor_t>::value,
               "socket_monitor_t must expose ignore_event");
static_assert (has_size_t<zlink::poller_t>::value, "poller_t must expose size");

template <typename T> class has_single_event_wait_t
{
  private:
    template <typename U>
    static auto test (int) -> decltype (std::declval<U &> ().wait (std::chrono::milliseconds (1)),
                                        std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

template <typename T> class has_vector_return_wait_t
{
  private:
    template <typename U>
    static auto test (int) -> decltype (std::declval<U &> ().wait (size_t (1),
                                                                   std::chrono::milliseconds (1)),
                                        std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

template <typename T> class has_tag_field_t
{
  private:
    template <typename U>
    static auto test (int) -> decltype (std::declval<U &> ().tag, std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

template <typename T> class has_raw_tag_field_t
{
  private:
    template <typename U>
    static auto test (int) -> decltype (std::declval<U &> ().raw_tag, std::true_type ());

    template <typename> static std::false_type test (...);

  public:
    static const bool value = decltype (test<T> (0))::value;
};

static_assert (!has_single_event_wait_t<zlink::poller_t>::value,
               "poller_t must not expose single-event wait");
static_assert (!has_vector_return_wait_t<zlink::poller_t>::value,
               "poller_t must not expose vector-return wait");
static_assert (!has_tag_field_t<zlink::poll_event_t>::value,
               "poll_event_t must not expose object tag");
static_assert (!has_raw_tag_field_t<zlink::poll_event_t>::value,
               "poll_event_t must not expose raw tag");

struct monitor_callback_state_t
{
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    zlink::monitor_event_t event;
};

void socket_monitor_callback (monitor_callback_state_t &state_,
                              const zlink::monitor_event_t &event_)
{
    {
        std::lock_guard<std::mutex> lock (state_.mutex);
        state_.event = event_;
        state_.ready = true;
    }
    state_.cv.notify_one ();
}

bool wait_for_any_socket_monitor_event (zlink::socket_monitor_t &monitor_, int timeout_ms_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);

    while (std::chrono::steady_clock::now () < deadline) {
        const std::chrono::steady_clock::duration remaining =
          deadline - std::chrono::steady_clock::now ();
        const int remaining_ms = static_cast<int> (
          std::chrono::duration_cast<std::chrono::milliseconds> (remaining).count ());
        if (!zlink_cpp_contract::wait_for_monitor_readable (monitor_, remaining_ms)) {
            continue;
        }

        if (monitor_.recv (zlink::recv_flags_t::dontwait))
            return true;
    }

    return false;
}

void test_socket_monitor_open_recv_snapshot ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t server (ctx);
    zlink::pair_socket_t client (ctx);

    zlink::socket_monitor_t monitor = server.monitor_open ();
    assert (monitor.valid ());

    server.bind ("tcp://127.0.0.1:*");
    std::string endpoint;
    endpoint = server.options ().last_endpoint ();
    assert (!endpoint.empty ());
    client.connect (endpoint);

    (void) monitor.recv (zlink::recv_flags_t::dontwait);
    assert (wait_for_any_socket_monitor_event (monitor, 2000));
    const zlink::monitor_status_t snapshot = monitor.status ();
    assert (snapshot.abi_version == ZLINK_MONITOR_STATUS_ABI_VERSION);
    assert (snapshot.struct_size == sizeof (zlink_monitor_status_t));
    (void) snapshot.auto_hwm_profile;
    (void) snapshot.auto_hwm_policy_class;
    (void) snapshot.auto_hwm_unit_budget_bytes;
    (void) snapshot.auto_hwm_size_cap;
    (void) snapshot.auto_hwm_socket_message_slots;
    (void) snapshot.auto_hwm_effective_sndbuf;
    (void) snapshot.auto_hwm_effective_rcvbuf;
    (void) snapshot.auto_hwm_planned_sndhwm_bytes;
    (void) snapshot.auto_hwm_planned_rcvhwm_bytes;
    (void) snapshot.auto_hwm_applied_sndhwm_bytes;
    (void) snapshot.auto_hwm_applied_rcvhwm_bytes;
    (void) snapshot.auto_hwm_deferred_sndhwm_valid;
    (void) snapshot.auto_hwm_deferred_rcvhwm_valid;
    (void) snapshot.snd_bytes_in_flight;
    (void) snapshot.rcv_bytes_in_flight;
}

void test_socket_monitor_ignore_event_and_poller_size ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t server (ctx);
    zlink::pair_socket_t client (ctx);
    zlink::socket_monitor_t monitor = server.monitor_open ();
    zlink::poller_t poller;

    poller.add (server, zlink::poll_event_flag_t::pollin, 1);
    assert (poller.size () == 1);

    monitor.on_event (zlink::socket_monitor_t::ignore_event);

    server.bind ("tcp://127.0.0.1:*");
    const std::string endpoint = server.options ().last_endpoint ();
    assert (!endpoint.empty ());
    client.connect (endpoint);
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (2000);
    bool ready = false;
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink::monitor_status_t snapshot = monitor.status ();
        if (snapshot.is_ready ()
            || (snapshot.state_flags & static_cast<uint32_t> (zlink::monitor_state::closed))
                 != 0u) {
            ready = true;
            break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    assert (ready);
    poller.remove (server);
    assert (poller.size () == 0);
}

void test_poller_wait_buffer_returns_slot ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t left (ctx);
    zlink::pair_socket_t right (ctx);
    zlink::socket_monitor_t left_monitor = left.monitor_open ();
    zlink::socket_monitor_t right_monitor = right.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("poller-tags");
    left.bind (endpoint);
    right.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      left_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      right_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    zlink::poller_t poller;
    std::vector<zlink::poll_event_t> events (1);
    poller.add (left, zlink::poll_event_flag_t::pollin, 17);

    zlink::message_t first = zlink_cpp_contract::make_message ("first");
    assert (right.send ().message (first).submit ());
    assert (poller.wait (events.data (), events.size (), std::chrono::milliseconds (2000)) == 1);
    assert (events[0].source_kind == zlink::poll_source_kind_t::socket);
    assert (events[0].slot == 17);
    assert (static_cast<short> (events[0].revents)
            & static_cast<short> (zlink::poll_event_flag_t::pollin));

    zlink::message_t inbound;
    assert (left.recv (inbound) == 0);
    poller.remove (left);

    zlink::message_t second = zlink_cpp_contract::make_message ("second");
    poller.add (left, zlink::poll_event_flag_t::pollin, 23);
    assert (right.send ().message (second).submit ());
    assert (poller.wait (events.data (), events.size (), std::chrono::milliseconds (2000)) == 1);
    assert (events[0].slot == 23);

    assert (left.recv (inbound) == 0);
}

void test_socket_only_poller_modify_rebuilds_cached_items ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t left (ctx);
    zlink::pair_socket_t right (ctx);
    zlink::socket_monitor_t left_monitor = left.monitor_open ();
    zlink::socket_monitor_t right_monitor = right.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("poller-socket-cache");
    left.bind (endpoint);
    right.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      left_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      right_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    zlink::poller_t poller;
    poller.add (left, zlink::poll_event_flag_t::none, 11);

    std::vector<zlink::poll_event_t> events (1);
    zlink::message_t first = zlink_cpp_contract::make_message ("first");
    assert (right.send ().message (first).submit ());
    assert (poller.wait (events.data (), events.size (), std::chrono::milliseconds (50)) == 0);

    poller.modify (left, zlink::poll_event_flag_t::pollin);
    assert (poller.wait (events.data (), events.size (), std::chrono::milliseconds (2000)) == 1);
    assert (events[0].slot == 11);
    assert (static_cast<short> (events[0].revents)
            & static_cast<short> (zlink::poll_event_flag_t::pollin));

    zlink::message_t inbound;
    assert (left.recv (inbound) == 0);
    assert (inbound.to_string () == "first");

    zlink::message_t second = zlink_cpp_contract::make_message ("second");
    assert (right.send ().message (second).submit ());
    assert (poller.wait (events.data (), events.size (), std::chrono::milliseconds (2000)) == 1);
    assert (events[0].slot == 11);
    assert (left.recv (inbound) == 0);
    assert (inbound.to_string () == "second");

    poller.modify (left, zlink::poll_event_flag_t::none);
    zlink::message_t third = zlink_cpp_contract::make_message ("third");
    assert (right.send ().message (third).submit ());
    assert (poller.wait (events.data (), events.size (), std::chrono::milliseconds (50)) == 0);

    poller.modify (left, zlink::poll_event_flag_t::pollin);
    assert (poller.wait (events.data (), events.size (), std::chrono::milliseconds (2000)) == 1);
    assert (events[0].slot == 11);
    assert (left.recv (inbound) == 0);
    assert (inbound.to_string () == "third");
}

void test_poller_capacity_leaves_remaining_ready_source ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t sender1 (ctx);
    zlink::pair_socket_t receiver1 (ctx);
    zlink::pair_socket_t sender2 (ctx);
    zlink::pair_socket_t receiver2 (ctx);

    const std::string endpoint1 = zlink_cpp_contract::unique_inproc ("poller-capacity-a");
    const std::string endpoint2 = zlink_cpp_contract::unique_inproc ("poller-capacity-b");
    sender1.bind (endpoint1);
    receiver1.connect (endpoint1);
    sender2.bind (endpoint2);
    receiver2.connect (endpoint2);

    zlink::poller_t poller;
    poller.add (receiver1, zlink::poll_event_flag_t::pollin, 101);
    poller.add (receiver2, zlink::poll_event_flag_t::pollin, 102);

    zlink::message_t first = zlink_cpp_contract::make_message ("a");
    zlink::message_t second = zlink_cpp_contract::make_message ("b");
    assert (sender1.send ().message (first).submit ());
    assert (sender2.send ().message (second).submit ());

    std::vector<zlink::poll_event_t> events (1);
    assert (poller.wait (events.data (), events.size (), std::chrono::milliseconds (2000)) == 1);
    const std::uintptr_t first_slot = events[0].slot;
    assert (first_slot == 101 || first_slot == 102);

    zlink::message_t inbound;
    if (first_slot == 101) {
        assert (receiver1.recv (inbound) == 0);
        assert (inbound.to_string () == "a");
    } else {
        assert (receiver2.recv (inbound) == 0);
        assert (inbound.to_string () == "b");
    }

    assert (poller.wait (events.data (), events.size (), std::chrono::milliseconds (2000)) == 1);
    assert (events[0].slot != first_slot);
    if (events[0].slot == 101) {
        assert (receiver1.recv (inbound) == 0);
        assert (inbound.to_string () == "a");
    } else {
        assert (receiver2.recv (inbound) == 0);
        assert (inbound.to_string () == "b");
    }
}

void test_poller_remove_suppresses_ready_events ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t sender (ctx);
    zlink::pair_socket_t receiver (ctx);
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("poller-remove");
    sender.bind (endpoint);
    receiver.connect (endpoint);

    zlink::poller_t poller;
    poller.add (receiver, zlink::poll_event_flag_t::pollin, 31);
    assert (poller.remove (receiver));

    zlink::message_t message = zlink_cpp_contract::make_message ("removed");
    assert (sender.send ().message (message).submit ());

    zlink::poll_event_t event;
    assert (poller.wait (&event, 1, std::chrono::milliseconds (0)) == 0);
}

void test_poller_distinguishes_timer_and_socket_in_same_buffer ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t sender (ctx);
    zlink::pair_socket_t receiver (ctx);
    zlink::timer_t timer;
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("poller-timer-socket");
    sender.bind (endpoint);
    receiver.connect (endpoint);

    zlink::poller_t poller;
    poller.add (receiver, zlink::poll_event_flag_t::pollin, 41);
    poller.add (timer, 42);

    zlink::message_t message = zlink_cpp_contract::make_message ("socket");
    assert (sender.send ().message (message).submit ());
    timer.start (std::chrono::milliseconds (5), 1);

    std::vector<zlink::poll_event_t> events (2);
    bool saw_socket = false;
    bool saw_timer = false;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while ((!saw_socket || !saw_timer) && std::chrono::steady_clock::now () < deadline) {
        const std::size_t count =
          poller.wait (events.data (), events.size (), std::chrono::milliseconds (200));
        for (std::size_t i = 0; i < count; ++i) {
            if (events[i].source_kind == zlink::poll_source_kind_t::socket) {
                assert (events[i].slot == 41);
                zlink::message_t inbound;
                assert (receiver.recv (inbound) == 0);
                assert (inbound.to_string () == "socket");
                saw_socket = true;
            } else if (events[i].source_kind == zlink::poll_source_kind_t::timer) {
                assert (events[i].slot == 42);
                const std::optional<uint64_t> fire_count = timer.recv ();
                assert (fire_count && *fire_count == 1);
                saw_timer = true;
            }
        }
    }
    assert (saw_socket);
    assert (saw_timer);
}

void test_socket_monitor_on_event_callback ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t server (ctx);
    zlink::pair_socket_t client (ctx);

    zlink::socket_monitor_t monitor = server.monitor_open ();
    assert (monitor.valid ());

    monitor_callback_state_t callback_state;
    monitor.on_event ([&callback_state] (const zlink::monitor_event_t &event) {
        socket_monitor_callback (callback_state, event);
    });

    server.bind ("tcp://127.0.0.1:*");
    std::string endpoint;
    endpoint = server.options ().last_endpoint ();
    assert (!endpoint.empty ());
    client.connect (endpoint);

    {
        std::unique_lock<std::mutex> lock (callback_state.mutex);
        assert (callback_state.cv.wait_for (lock, std::chrono::seconds (2),
                                            [&callback_state] { return callback_state.ready; }));
    }
    assert (static_cast<uint64_t> (callback_state.event.event) != 0u);

    bool monitor_status_ready = false;
    const std::chrono::steady_clock::time_point monitor_status_deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (500);
    while (std::chrono::steady_clock::now () < monitor_status_deadline) {
        const zlink::monitor_status_t snapshot = monitor.status ();
        if (snapshot.is_ready ()
            || (snapshot.state_flags & static_cast<uint32_t> (zlink::monitor_state::closed))
                 != 0u) {
            monitor_status_ready = true;
            break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    assert (monitor_status_ready);
}

} // namespace

int main ()
{
    test_socket_monitor_open_recv_snapshot ();
    test_socket_monitor_ignore_event_and_poller_size ();
    test_poller_wait_buffer_returns_slot ();
    test_socket_only_poller_modify_rebuilds_cached_items ();
    test_poller_capacity_leaves_remaining_ready_source ();
    test_poller_remove_suppresses_ready_events ();
    test_poller_distinguishes_timer_and_socket_in_same_buffer ();
    test_socket_monitor_on_event_callback ();
    return 0;
}
