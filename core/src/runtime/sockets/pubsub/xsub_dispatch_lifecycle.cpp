/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/pubsub/xsub.hpp"
#include "sockets/pubsub/xsub_dispatch_internal.hpp"
#include "core/io_thread.hpp"

int zlink::xsub_t::sub_dispatch_start (sub_io_handler_fn callback_, void *userdata_)
{
    if (!callback_) {
        errno = EINVAL;
        return -1;
    }

    io_thread_t *io_thread = choose_io_thread (options.affinity);
    if (!io_thread) {
        errno = EAGAIN;
        return -1;
    }

    std::lock_guard<std::mutex> lk (_dispatch_control_mu);
    if (_dispatch_active.load (std::memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }

    _dispatch_userdata.store (userdata_, std::memory_order_release);
    _dispatch_callback.store (callback_, std::memory_order_release);
    _dispatch_active.store (true, std::memory_order_release);
    if (start_async_mailbox_processing (io_thread) != 0) {
        _dispatch_active.store (false, std::memory_order_release);
        _dispatch_callback.store (NULL, std::memory_order_release);
        _dispatch_userdata.store (NULL, std::memory_order_release);
        return -1;
    }
    return 0;
}

int zlink::xsub_t::sub_dispatch_stop ()
{
    bool wait_for_callbacks = true;
    {
        std::lock_guard<std::mutex> lk (_dispatch_control_mu);
        if (!_dispatch_active.load (std::memory_order_acquire)) {
            errno = EINVAL;
            return -1;
        }

        _dispatch_active.store (false, std::memory_order_release);
        _dispatch_callback.store (NULL, std::memory_order_release);
        _dispatch_userdata.store (NULL, std::memory_order_release);
        wait_for_callbacks = !zlink::xsub_dispatch_owns_socket (this);
    }
    stop_async_mailbox_processing ();
    wait_async_quiesced (10000);

    if (!wait_for_callbacks)
        return 0;

    std::unique_lock<std::mutex> lk (_dispatch_inflight_mu);
    while (_dispatch_inflight.load (std::memory_order_acquire) > 0)
        _dispatch_inflight_cv.wait (lk);
    return 0;
}

bool zlink::xsub_t::sub_dispatch_active () const
{
    return _dispatch_active.load (std::memory_order_acquire);
}

void zlink::xsub_t::notify_dispatch_stopped ()
{
    const uint32_t remaining = _dispatch_inflight.fetch_sub (1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) {
        std::lock_guard<std::mutex> lk (_dispatch_inflight_mu);
        _dispatch_inflight_cv.notify_all ();
    }
}
