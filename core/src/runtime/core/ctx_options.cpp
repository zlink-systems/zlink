/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/auto_hwm_policy.hpp"
#include "core/ctx.hpp"

#include <climits>
#include <string.h>

int zlink::ctx_t::set (int option_, const void *optval_, size_t optvallen_)
{
    const bool is_int = (optvallen_ == sizeof (int));
    int value = 0;
    if (is_int)
        memcpy (&value, optval_, sizeof (int));

    bool refresh_auto_hwm = false;

    switch (option_) {
        case ZLINK_MAX_SOCKETS:
            if (is_int && value >= 1 && value == clipped_maxsocket (value)) {
                scoped_lock_t locker (_opt_sync);
                _max_sockets = value;
                return 0;
            }
            break;

        case ZLINK_IO_THREADS:
            if (is_int && value >= 0) {
                scoped_lock_t locker (_opt_sync);
                _io_thread_count = value;
                return 0;
            }
            break;

        case ZLINK_CTX_OPT_AUTO_HWM_ENABLE:
            if (is_int && (value == 0 || value == 1)) {
                scoped_lock_t locker (_opt_sync);
                _auto_hwm.set_enabled (value != 0);
                refresh_auto_hwm = true;
                break;
            }
            break;

        case ZLINK_CTX_OPT_AUTO_HWM_RECALC_DEBOUNCE_MS:
            if (is_int && value >= 0) {
                scoped_lock_t locker (_opt_sync);
                _auto_hwm.set_recalc_debounce_ms (value);
                refresh_auto_hwm = true;
                break;
            }
            break;

        case ZLINK_CTX_OPT_AUTO_HWM_PROFILE:
            if (is_int
                && (value == ZLINK_AUTO_HWM_PROFILE_COMPACT
                    || value == ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY
                    || value == ZLINK_AUTO_HWM_PROFILE_BALANCED
                    || value == ZLINK_AUTO_HWM_PROFILE_THROUGHPUT)) {
                scoped_lock_t locker (_opt_sync);
                _auto_hwm.set_profile (static_cast<zlink_auto_hwm_profile_t> (value));
                refresh_auto_hwm = true;
                break;
            }
            break;

        case ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES:
            if (optvallen_ == sizeof (uint64_t)) {
                uint64_t msg_unit_bytes = 0;
                memcpy (&msg_unit_bytes, optval_, sizeof (msg_unit_bytes));
                scoped_lock_t locker (_opt_sync);
                _auto_hwm.set_msg_unit_bytes (msg_unit_bytes);
                refresh_auto_hwm = true;
                break;
            }
            break;

        case ZLINK_INTERNAL_OPT_IPV6:
            if (is_int && value >= 0) {
                scoped_lock_t locker (_opt_sync);
                _ipv6 = (value != 0);
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_BLOCKY:
        case ZLINK_CTX_OPT_BLOCKY:
            if (is_int && value >= 0) {
                scoped_lock_t locker (_opt_sync);
                _blocky = (value != 0);
                return 0;
            }
            break;

        case ZLINK_MAX_MSGSZ:
            if (is_int && value >= 0) {
                scoped_lock_t locker (_opt_sync);
                _max_msgsz = value < INT_MAX ? value : INT_MAX;
                return 0;
            }
            break;

        default:
            return _thread_context.set (option_, optval_, optvallen_);
    }

    if (refresh_auto_hwm) {
        schedule_auto_hwm_recalculate ();
        return 0;
    }

    errno = EINVAL;
    return -1;
}

int zlink::ctx_t::get (int option_, void *optval_, const size_t *optvallen_)
{
    const bool is_int = (*optvallen_ == sizeof (int));
    int *value = static_cast<int *> (optval_);

    switch (option_) {
        case ZLINK_MAX_SOCKETS:
            if (is_int) {
                scoped_lock_t locker (_opt_sync);
                *value = _max_sockets;
                return 0;
            }
            break;

        case ZLINK_SOCKET_LIMIT:
            if (is_int) {
                *value = clipped_maxsocket (65535);
                return 0;
            }
            break;

        case ZLINK_IO_THREADS:
            if (is_int) {
                scoped_lock_t locker (_opt_sync);
                *value = _io_thread_count;
                return 0;
            }
            break;

        case ZLINK_CTX_OPT_AUTO_HWM_ENABLE:
            if (is_int) {
                scoped_lock_t locker (_opt_sync);
                *value = _auto_hwm.enabled () ? 1 : 0;
                return 0;
            }
            break;

        case ZLINK_CTX_OPT_AUTO_HWM_RECALC_DEBOUNCE_MS:
            if (is_int) {
                scoped_lock_t locker (_opt_sync);
                *value = _auto_hwm.recalc_debounce_ms ();
                return 0;
            }
            break;

        case ZLINK_CTX_OPT_AUTO_HWM_PROFILE:
            if (is_int) {
                scoped_lock_t locker (_opt_sync);
                *value = _auto_hwm.profile ();
                return 0;
            }
            break;

        case ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES:
            if (*optvallen_ == sizeof (uint64_t)) {
                scoped_lock_t locker (_opt_sync);
                const uint64_t msg_unit_bytes = _auto_hwm.msg_unit_bytes ();
                memcpy (optval_, &msg_unit_bytes, sizeof (msg_unit_bytes));
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_IPV6:
            if (is_int) {
                scoped_lock_t locker (_opt_sync);
                *value = _ipv6;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_BLOCKY:
        case ZLINK_CTX_OPT_BLOCKY:
            if (is_int) {
                scoped_lock_t locker (_opt_sync);
                *value = _blocky;
                return 0;
            }
            break;

        case ZLINK_MAX_MSGSZ:
            if (is_int) {
                scoped_lock_t locker (_opt_sync);
                *value = _max_msgsz;
                return 0;
            }
            break;

        case ZLINK_MSG_T_SIZE:
            if (is_int) {
                scoped_lock_t locker (_opt_sync);
                *value = sizeof (zlink_msg_t);
                return 0;
            }
            break;

        default:
            return _thread_context.get (option_, optval_, optvallen_);
    }

    errno = EINVAL;
    return -1;
}

int zlink::ctx_t::get (int option_)
{
    int optval = 0;
    size_t optvallen = sizeof (int);

    if (get (option_, &optval, &optvallen) == 0)
        return optval;

    errno = EINVAL;
    return -1;
}
