/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework/contracts/configuration/services.hpp>

#include <atomic>
#include <barrier>
#include <future>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace
{
using namespace zlink::framework;

constexpr int concurrent_resolution_iterations = 20;
constexpr int cached_resolution_checks = 256;

struct observation_t
{
    std::atomic<void *> first_once{nullptr};
    std::atomic<int> once_entries{0};
    std::atomic<bool> second_signalled{false};
    std::latch second_observed{1};

    void signal_second ()
    {
        if (!second_signalled.exchange (true)) {
            // A nested promise completion would overwrite libstdc++'s
            // call_once TLS before pthread_once invokes its callable.
            second_observed.count_down ();
        }
    }
};

thread_local bool observe_resolution = false;
// Set before worker creation and cleared after both joins.
observation_t *observation = nullptr;
std::atomic<int> once_calls{0};
std::atomic<int> mutex_calls{0};

struct singleton_t
{
};

bool check (bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}
} // namespace

#if defined(__linux__)
extern "C" int __real_pthread_once (pthread_once_t *, void (*) ());
extern "C" int __real_pthread_mutex_lock (pthread_mutex_t *);

extern "C" int __wrap_pthread_once (pthread_once_t *flag, void (*function) ())
{
    if (observe_resolution) {
        ++once_calls;
        if (observation) {
            void *expected = nullptr;
            if (observation->first_once.compare_exchange_strong (expected, flag)
                || expected == flag) {
                if (observation->once_entries.fetch_add (1) == 1) {
                    observation->signal_second ();
                }
            }
        }
    }
    return __real_pthread_once (flag, function);
}

extern "C" int __wrap_pthread_mutex_lock (pthread_mutex_t *mutex)
{
    if (observe_resolution) {
        ++mutex_calls;
    }
    return __real_pthread_mutex_lock (mutex);
}
#endif

namespace
{
bool synchronization_probes ()
{
#if defined(__linux__)
    const int once_before = once_calls;
    const int mutex_before = mutex_calls;
    std::once_flag once;
    std::mutex mutex;
    observe_resolution = true;
    std::call_once (once, [] {});
    mutex.lock ();
    mutex.unlock ();
    observe_resolution = false;
    const int observed_once = once_calls - once_before;
    const int observed_mutex = mutex_calls - mutex_before;
    std::cout << "native_probe once=" << observed_once << " mutex=" << observed_mutex << '\n';
    return check (observed_once == 1 && observed_mutex == 1,
                  "native synchronization counters observe positive probes");
#else
    return true;
#endif
}

bool concurrent_first_resolution ()
{
    service_collection_t services;
    observation_t events;
    observation = &events;
    std::promise<void> first_entered, release_first, release_second, first_completed;
    auto entered = first_entered.get_future ();
    auto first_release = release_first.get_future ().share ();
    auto second_release = release_second.get_future ().share ();
    auto completed = first_completed.get_future ();
    std::atomic<int> factory_calls{0};
    std::atomic<bool> completion_signalled{false};

    services.add_factory<singleton_t> (
      [&] (service_provider_t &) {
          const auto ordinal = factory_calls.fetch_add (1);
          if (ordinal == 0) {
              first_entered.set_value ();
              first_release.wait ();
          } else {
              // The old resolver reaches a second factory. The fixed resolver
              // reaches the same pthread_once instead; either observation lets
              // main release the first factory without a clock-based deadline.
              events.signal_second ();
              second_release.wait ();
          }
          return std::make_unique<singleton_t> ();
      },
      service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    singleton_t *values[2]{};
    std::barrier start{3};
    auto resolve = [&] (int index) {
        start.arrive_and_wait ();
        observe_resolution = true;
        values[index] = &provider.get_required<singleton_t> ();
        observe_resolution = false;
        if (!completion_signalled.exchange (true)) {
            first_completed.set_value ();
        }
    };
    std::thread first (resolve, 0), second (resolve, 1);
    start.arrive_and_wait ();
    entered.wait ();
#if defined(__linux__)
    events.second_observed.wait ();
#endif
    release_first.set_value ();
    completed.wait ();
    release_second.set_value ();
    first.join ();
    second.join ();
    observation = nullptr;

    bool success = check (values[0] == values[1], "concurrent singleton identity");
    success = check (factory_calls == 1, "concurrent factory is called once") && success;
    const int once_before = once_calls;
    const int mutex_before = mutex_calls;
    bool stable = true;
    observe_resolution = true;
    for (int i = 0; i < cached_resolution_checks; ++i) {
        stable = (&provider.get_required<singleton_t> () == values[0]) && stable;
    }
    observe_resolution = false;
    const int warm_once = once_calls - once_before;
    const int warm_mutex = mutex_calls - mutex_before;
    success = check (stable && factory_calls == 1, "warm singleton identity") && success;
    success = check (warm_once == 0 && warm_mutex == 0,
                     "warm lookup bypasses pthread_once and mutex acquisition")
              && success;
    std::cout << "concurrent factory_calls=" << factory_calls
              << " same_instance=" << (values[0] == values[1]) << " warm_once=" << warm_once
              << " warm_mutex=" << warm_mutex << '\n';
    return success;
}

bool factory_failure_retries ()
{
    service_collection_t services;
    int attempts = 0;
    services.add_factory<singleton_t> (
      [&] (service_provider_t &) {
          if (++attempts == 1) {
              throw std::runtime_error ("factory failure");
          }
          return std::make_unique<singleton_t> ();
      },
      service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    bool propagated = false;
    try {
        provider.get_required<singleton_t> ();
    }
    catch (const std::runtime_error &error) {
        propagated = std::string (error.what ()) == "factory failure";
    }
    auto *first = &provider.get_required<singleton_t> ();
    auto *second = &provider.get_required<singleton_t> ();
    return check (propagated && attempts == 2 && first == second,
                  "factory exception propagates, next resolve retries, success is cached");
}

bool recursive_resolution_retries ()
{
    service_collection_t services;
    bool recurse = true;
    int calls = 0;
    services.add_factory<singleton_t> (
      [&] (service_provider_t &provider) {
          ++calls;
          if (recurse) {
              // Bound the old resolver's recursive path so its negative test
              // also exits through normal unwinding, without a stack overflow.
              if (calls > 1) {
                  throw std::runtime_error ("unguarded recursion");
              }
              provider.get_required<singleton_t> ();
          }
          return std::make_unique<singleton_t> ();
      },
      service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    bool rejected = false;
    try {
        provider.get_required<singleton_t> ();
    }
    catch (const std::logic_error &) {
        rejected = true;
    }
    catch (const std::runtime_error &error) {
        std::cout << "old recursion: " << error.what () << '\n';
    }
    const auto recursive_calls = calls;
    recurse = false;
    auto *first = &provider.get_required<singleton_t> ();
    auto *second = &provider.get_required<singleton_t> ();
    return check (rejected && recursive_calls == 1 && calls == 2 && first == second,
                  "recursive resolve fails before factory reentry and later resolve retries");
}
struct cycle_dependency_t
{
};

bool indirect_recursive_resolution_retries ()
{
    service_collection_t services;
    bool recurse = true;
    int singleton_calls = 0;
    services.add_factory<singleton_t> (
      [&] (service_provider_t &provider) {
          if (++singleton_calls > 1 && recurse) {
              throw std::runtime_error ("unguarded indirect recursion");
          }
          provider.get_required<cycle_dependency_t> ();
          return std::make_unique<singleton_t> ();
      },
      service_lifetime_t::singleton);
    services.add_factory<cycle_dependency_t> (
      [&] (service_provider_t &provider) {
          if (recurse) {
              provider.get_required<singleton_t> ();
          }
          return std::make_unique<cycle_dependency_t> ();
      },
      service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    bool rejected = false;
    try {
        provider.get_required<singleton_t> ();
    }
    catch (const std::logic_error &) {
        rejected = true;
    }
    catch (const std::runtime_error &error) {
        std::cout << "old indirect recursion: " << error.what () << '\n';
    }
    const auto recursive_calls = singleton_calls;
    recurse = false;
    auto *first = &provider.get_required<singleton_t> ();
    auto *second = &provider.get_required<singleton_t> ();
    return check (rejected && recursive_calls == 1 && singleton_calls == 2 && first == second,
                  "indirect recursion unwinds both registrations and permits a later retry");
}

enum class released_t
{
    prebuilt,
    dependency,
    dependent
};

template <released_t Tag> struct owned_t
{
    explicit owned_t (std::vector<released_t> &events_) : events (events_) {}
    ~owned_t () { events.push_back (Tag); }
    std::vector<released_t> &events;
};

bool singleton_ownership_order ()
{
    using prebuilt_t = owned_t<released_t::prebuilt>;
    using dependency_t = owned_t<released_t::dependency>;
    using dependent_t = owned_t<released_t::dependent>;
    std::vector<released_t> released;
    bool prebuilt_identity = false;
    {
        service_collection_t services;
        auto instance = std::make_unique<prebuilt_t> (released);
        auto *expected = instance.get ();
        services.add_singleton<prebuilt_t> (std::move (instance));
        services.add_factory<dependent_t> (
          [&] (service_provider_t &provider) {
              provider.get_required<dependency_t> ();
              return std::make_unique<dependent_t> (released);
          },
          service_lifetime_t::singleton);
        services.add_factory<dependency_t> (
          [&] (service_provider_t &) { return std::make_unique<dependency_t> (released); },
          service_lifetime_t::singleton);
        auto provider = services.build_provider ();
        prebuilt_identity = &provider.get_required<prebuilt_t> () == expected;
        provider.get_required<dependent_t> ();
    }
    return check (prebuilt_identity
                    && released
                         == std::vector<released_t>{released_t::dependent, released_t::dependency,
                                                    released_t::prebuilt},
                  "prebuilt identity and reverse acquisition order are preserved");
}
struct other_singleton_t
{
};

bool distinct_singleton_factories_overlap ()
{
    service_collection_t services;
    std::latch factories_entered{2};
    std::promise<void> release_first, release_second, first_completed;
    auto first_release = release_first.get_future ().share ();
    auto second_release = release_second.get_future ().share ();
    auto completed = first_completed.get_future ();
    std::atomic<int> factory_calls{0};
    std::atomic<bool> completion_signalled{false};
    auto await_release = [&] {
        const auto ordinal = factory_calls.fetch_add (1);
        factories_entered.count_down ();
        (ordinal == 0 ? first_release : second_release).wait ();
    };
    services.add_factory<singleton_t> (
      [&] (service_provider_t &) {
          await_release ();
          return std::make_unique<singleton_t> ();
      },
      service_lifetime_t::singleton);
    services.add_factory<other_singleton_t> (
      [&] (service_provider_t &) {
          await_release ();
          return std::make_unique<other_singleton_t> ();
      },
      service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    singleton_t *first_value = nullptr;
    other_singleton_t *second_value = nullptr;
    std::barrier start{3};
    auto resolve = [&]<typename T> (T *&value) {
        start.arrive_and_wait ();
        value = &provider.get_required<T> ();
        if (!completion_signalled.exchange (true)) {
            first_completed.set_value ();
        }
    };
    std::thread first ([&] { resolve (first_value); });
    std::thread second ([&] { resolve (second_value); });
    start.arrive_and_wait ();
    // Both factories must enter before either can publish. Order publication
    // so this test is also safe for the original unsynchronized instance store.
    factories_entered.wait ();
    release_first.set_value ();
    completed.wait ();
    release_second.set_value ();
    first.join ();
    second.join ();
    const bool stable = &provider.get_required<singleton_t> () == first_value
                        && &provider.get_required<other_singleton_t> () == second_value;
    std::cout << "distinct factory_calls=" << factory_calls << " stable=" << stable << '\n';
    return check (factory_calls == 2 && stable,
                  "distinct singleton factories overlap and retain their identities");
}

} // namespace

int main ()
{
    if (!synchronization_probes ()) {
        return 1;
    }
    bool success = true;
    for (int i = 0; i < concurrent_resolution_iterations; ++i) {
        success = concurrent_first_resolution () && success;
    }
    success = distinct_singleton_factories_overlap () && success;
    success = factory_failure_retries () && success;
    success = recursive_resolution_retries () && success;
    success = indirect_recursive_resolution_retries () && success;
    success = singleton_ownership_order () && success;
    return success ? 0 : 1;
}
