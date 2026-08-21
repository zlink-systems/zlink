/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CTX_IO_THREAD_REGISTRY_HPP_INCLUDED__
#define __ZLINK_CTX_IO_THREAD_REGISTRY_HPP_INCLUDED__

#include <vector>

#include "utils/atomic_counter.hpp"
#include "utils/stdint.hpp"

namespace zlink
{
class io_thread_t;

class ctx_io_thread_registry_t
{
  public:
    ctx_io_thread_registry_t ();

    void clear ();
    void add (io_thread_t *io_thread_);
    void stop_all ();
    void destroy_all ();

    io_thread_t *choose (uint64_t affinity_);
    io_thread_t *choose_transport (uint64_t affinity_);
    io_thread_t *choose_stream (uint64_t affinity_);

  private:
    typedef std::vector<io_thread_t *> io_threads_t;

    io_threads_t _threads;
    atomic_counter_t _next_io_thread;
    atomic_counter_t _next_transport_io_thread;
    atomic_counter_t _next_stream_io_thread;
};
}

#endif
