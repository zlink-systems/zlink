/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_submit_retry_fault_injection.hpp"

#ifdef ZLINK_BUILD_TESTS
#include <atomic>

namespace
{
std::atomic<int> g_submit_retry_faults_remaining (0);
std::atomic<int> g_submit_retry_fault_errno (ENOTCONN);
}

extern "C" void zlink_test_set_submit_retry_fault (int count_, int err_)
{
    g_submit_retry_fault_errno.store (err_ == 0 ? ENOTCONN : err_, std::memory_order_relaxed);
    g_submit_retry_faults_remaining.store (count_ < 0 ? 0 : count_, std::memory_order_relaxed);
}

bool zlink::socket_submit_retry_fault::pending ()
{
    return g_submit_retry_faults_remaining.load (std::memory_order_relaxed)
           > 0;
}

bool zlink::socket_submit_retry_fault::consume (int *err_out_)
{
    int remaining = g_submit_retry_faults_remaining.load (std::memory_order_relaxed);
    while (remaining > 0) {
        if (g_submit_retry_faults_remaining.compare_exchange_weak (remaining, remaining - 1,
                                                                   std::memory_order_relaxed)) {
            if (err_out_)
                *err_out_ = g_submit_retry_fault_errno.load (std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}
#endif
