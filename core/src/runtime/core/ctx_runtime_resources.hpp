/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CTX_RUNTIME_RESOURCES_HPP_INCLUDED__
#define __ZLINK_CTX_RUNTIME_RESOURCES_HPP_INCLUDED__

#include "core/ctx_io_thread_registry.hpp"

namespace zlink
{
class ctx_t;
class ctx_socket_registry_t;
class mailbox_t;
class object_t;
class reaper_t;
class control_runtime_t;

class ctx_runtime_resources_t
{
  public:
    ctx_runtime_resources_t ();

    bool start_locked (ctx_t &ctx_,
                       ctx_socket_registry_t &socket_registry_,
                       mailbox_t &term_mailbox_,
                       int max_sockets_,
                       int io_thread_count_);
    void teardown (ctx_t &ctx_, ctx_socket_registry_t &socket_registry_);

    control_runtime_t *control_runtime () const;
    object_t *reaper_object () const;
    void stop_reaper ();

    io_thread_t *choose_io_thread (uint64_t affinity_);
    io_thread_t *choose_io_thread_stream (uint64_t affinity_);

  private:
    void cleanup_failed_start_locked (ctx_t &ctx_, ctx_socket_registry_t &socket_registry_);
    bool start_reaper_locked (ctx_t &ctx_, ctx_socket_registry_t &socket_registry_);
    bool start_control_runtime_locked (ctx_t &ctx_);
    bool start_io_threads_locked (ctx_t &ctx_,
                                  ctx_socket_registry_t &socket_registry_,
                                  int io_thread_count_);

    reaper_t *_reaper;
    control_runtime_t *_control_runtime;
    ctx_io_thread_registry_t _io_thread_registry;
};
}

#endif
