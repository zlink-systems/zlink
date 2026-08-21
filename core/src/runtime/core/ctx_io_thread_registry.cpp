/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <string.h>

#include "core/ctx_io_thread_registry.hpp"

#include "core/io_thread.hpp"
#include "utils/env.hpp"

namespace
{
enum stream_sched_mode_t
{
    stream_sched_minload = 0,
    stream_sched_rr = 1
};

stream_sched_mode_t parse_stream_session_sched_mode ()
{
    if (zlink::env::equals ("ZLINK_ASIO_STREAM_SESSION_SCHED", "rr", "RR"))
        return stream_sched_rr;
    if (zlink::env::equals ("ZLINK_ASIO_STREAM_SESSION_SCHED", "minload", "MINLOAD"))
        return stream_sched_minload;

    return stream_sched_rr;
}
}

zlink::ctx_io_thread_registry_t::ctx_io_thread_registry_t () :
    _next_io_thread (0), _next_transport_io_thread (0),
    _next_stream_io_thread (0)
{
}

void zlink::ctx_io_thread_registry_t::clear ()
{
    _threads.clear ();
    _next_io_thread.set (0);
    _next_transport_io_thread.set (0);
    _next_stream_io_thread.set (0);
}

void zlink::ctx_io_thread_registry_t::add (io_thread_t *io_thread_)
{
    _threads.push_back (io_thread_);
}

void zlink::ctx_io_thread_registry_t::stop_all ()
{
    const io_threads_t::size_type size = _threads.size ();
    for (io_threads_t::size_type i = 0; i != size; ++i)
        _threads[i]->stop ();
}

void zlink::ctx_io_thread_registry_t::destroy_all ()
{
    const io_threads_t::size_type size = _threads.size ();
    for (io_threads_t::size_type i = 0; i != size; ++i)
        LIBZLINK_DELETE (_threads[i]);
    clear ();
}

zlink::io_thread_t *zlink::ctx_io_thread_registry_t::choose (uint64_t affinity_)
{
    if (_threads.empty ())
        return NULL;

    const io_threads_t::size_type size = _threads.size ();
    const io_threads_t::size_type start_index =
      static_cast<io_threads_t::size_type> (_next_io_thread.add (1) % size);

    int min_load = -1;
    io_thread_t *selected_io_thread = NULL;
    for (io_threads_t::size_type n = 0; n != size; ++n) {
        const io_threads_t::size_type i = (start_index + n) % size;
        if (!affinity_ || (affinity_ & (uint64_t (1) << i))) {
            const int load = _threads[i]->get_load ();
            if (selected_io_thread == NULL || load < min_load) {
                min_load = load;
                selected_io_thread = _threads[i];
            }
        }
    }

    return selected_io_thread;
}

zlink::io_thread_t *
zlink::ctx_io_thread_registry_t::choose_transport (uint64_t affinity_)
{
    if (_threads.empty ())
        return NULL;

    const io_threads_t::size_type size = _threads.size ();
    const io_threads_t::size_type start_index =
      static_cast<io_threads_t::size_type> (
        _next_transport_io_thread.add (1) % size);
    for (io_threads_t::size_type n = 0; n != size; ++n) {
        const io_threads_t::size_type i = (start_index + n) % size;
        if (!affinity_ || (affinity_ & (uint64_t (1) << i)))
            return _threads[i];
    }
    return NULL;
}

zlink::io_thread_t *zlink::ctx_io_thread_registry_t::choose_stream (uint64_t affinity_)
{
    if (_threads.empty ())
        return NULL;

    static const stream_sched_mode_t mode = parse_stream_session_sched_mode ();
    if (mode == stream_sched_rr) {
        const io_threads_t::size_type size = _threads.size ();
        const io_threads_t::size_type start_index =
          static_cast<io_threads_t::size_type> (_next_stream_io_thread.add (1) % size);

        for (io_threads_t::size_type n = 0; n != size; ++n) {
            const io_threads_t::size_type i = (start_index + n) % size;
            if (!affinity_ || (affinity_ & (uint64_t (1) << i)))
                return _threads[i];
        }

        return NULL;
    }

    return choose (affinity_);
}
