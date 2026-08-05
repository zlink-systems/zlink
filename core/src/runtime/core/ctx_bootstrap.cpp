/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#ifndef ZLINK_HAVE_WINDOWS
#include <unistd.h>
#endif

#include "core/ctx.hpp"
#include "core/control_runtime.hpp"

namespace
{
const int ctx_bootstrap_retry_count = 50;
}

bool zlink::ctx_t::start_runtime_locked ()
{
    _opt_sync.lock ();
    const int max_sockets = _max_sockets;
    const int ios = _io_thread_count;
    _opt_sync.unlock ();

    if (!_runtime_resources.start_locked (*this, _socket_registry, _term_mailbox, max_sockets, ios))
        return false;

    _starting = false;
    return true;
}

zlink::control_runtime_t *zlink::ctx_t::ensure_control_runtime ()
{
    int last_errno = ENOTSUP;
    for (int attempt = 0; attempt < ctx_bootstrap_retry_count; ++attempt) {
        _slot_sync.lock ();
        if (_terminating) {
            _slot_sync.unlock ();
            errno = ETERM;
            return NULL;
        }
        control_runtime_t *runtime = _runtime_resources.control_runtime ();
        if (runtime) {
            _slot_sync.unlock ();
            return runtime;
        }
        if (!_starting) {
            _slot_sync.unlock ();
            errno = ENOTSUP;
            return NULL;
        }

        const bool started = start_runtime_locked ();
        runtime = _runtime_resources.control_runtime ();
        last_errno = errno;
        _slot_sync.unlock ();
        if (started && runtime)
            return runtime;
#ifndef ZLINK_HAVE_WINDOWS
        usleep (1000);
#endif
    }

    errno = last_errno;
    return NULL;
}
