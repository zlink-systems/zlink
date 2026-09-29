/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/execution/state_lane.hpp"

#include <condition_variable>
#include <thread>
#include <utility>

namespace zlink::framework::runtime::infrastructure_wait_guard
{
enum class wait_relation_t
{
    own_input,
    dependent_completion
};

#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
template <typename Ready>
bool before_condition_wait (wait_relation_t relation, Ready &&ready, const char *site)
{
    if (relation == wait_relation_t::dependent_completion
        && ::zlink::framework::detail::current_infrastructure_wait_owner) {
        ::zlink::framework::detail::check_infrastructure_wait (ready (), site);
        return true;
    }
    return false;
}
#endif

#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
// The scope points at an existing infrastructure owner; it owns no additional lifetime.
class infrastructure_scope_t
{
  public:
    explicit infrastructure_scope_t (const void *owner) noexcept :
        _previous (::zlink::framework::detail::current_infrastructure_wait_owner)
    {
        ::zlink::framework::detail::current_infrastructure_wait_owner = owner;
    }
    ~infrastructure_scope_t ()
    {
        ::zlink::framework::detail::current_infrastructure_wait_owner = _previous;
    }

  private:
    const void *_previous;
};
using mesh_receive_scope_t = infrastructure_scope_t;

inline void before_join (const std::thread &worker, const char *site)
{
    if (worker.joinable () && worker.get_id () == std::this_thread::get_id ())
        ::zlink::framework::detail::fail_infrastructure_wait (site);
}
#else
class infrastructure_scope_t
{
  public:
    explicit infrastructure_scope_t (const void *) noexcept {}
};
using mesh_receive_scope_t = infrastructure_scope_t;

inline void before_join (const std::thread &, const char *) noexcept
{
}
#endif

inline void join (std::thread &worker, const char *site)
{
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
    before_join (worker, site);
#else
    (void) site;
#endif
    worker.join ();
}

template <typename Condition, typename Lock, typename Predicate>
void condition_wait (Condition &condition,
                     Lock &lock,
                     Predicate &&predicate,
                     const char *site,
                     wait_relation_t relation)
{
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
    if (before_condition_wait (relation, predicate, site))
        return;
#else
    (void) site;
    (void) relation;
#endif
    condition.wait (lock, std::forward<Predicate> (predicate));
}

template <typename Condition, typename Lock, typename Clock, typename Duration, typename Predicate>
bool condition_wait_until (Condition &condition,
                           Lock &lock,
                           const std::chrono::time_point<Clock, Duration> &deadline,
                           Predicate &&predicate,
                           const char *site,
                           wait_relation_t relation)
{
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
    if (deadline > Clock::now () && before_condition_wait (relation, predicate, site))
        return true;
#else
    (void) site;
    (void) relation;
#endif
    return condition.wait_until (lock, deadline, std::forward<Predicate> (predicate));
}

template <typename Condition, typename Lock, typename Clock, typename Duration>
std::cv_status condition_wait_until (Condition &condition,
                                     Lock &lock,
                                     const std::chrono::time_point<Clock, Duration> &deadline,
                                     const char *site,
                                     wait_relation_t relation)
{
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
    if (deadline > Clock::now ())
        before_condition_wait (relation, [] { return false; }, site);
#else
    (void) site;
    (void) relation;
#endif
    return condition.wait_until (lock, deadline);
}

template <typename Condition, typename Lock, typename Rep, typename Period, typename Predicate>
bool condition_wait_for (Condition &condition,
                         Lock &lock,
                         const std::chrono::duration<Rep, Period> &timeout,
                         Predicate &&predicate,
                         const char *site,
                         wait_relation_t relation)
{
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
    if (timeout > timeout.zero () && before_condition_wait (relation, predicate, site))
        return true;
#else
    (void) site;
    (void) relation;
#endif
    return condition.wait_for (lock, timeout, std::forward<Predicate> (predicate));
}

template <typename Condition, typename Lock, typename Rep, typename Period>
std::cv_status condition_wait_for (Condition &condition,
                                   Lock &lock,
                                   const std::chrono::duration<Rep, Period> &timeout,
                                   const char *site,
                                   wait_relation_t relation)
{
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
    if (timeout > timeout.zero ())
        before_condition_wait (relation, [] { return false; }, site);
#else
    (void) site;
    (void) relation;
#endif
    return condition.wait_for (lock, timeout);
}
} // namespace zlink::framework::runtime::infrastructure_wait_guard
