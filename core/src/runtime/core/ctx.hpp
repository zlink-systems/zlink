/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CTX_HPP_INCLUDED__
#define __ZLINK_CTX_HPP_INCLUDED__

#include <map>
#include <vector>
#include <string>
#include <stdarg.h>

#include "core/mailbox.hpp"
#include "core/ctx_auto_hwm_state.hpp"
#include "core/ctx_inproc_registry.hpp"
#include "core/ctx_runtime_resources.hpp"
#include "core/ctx_socket_registry.hpp"
#include "core/ctx_thread.hpp"
#include "utils/array.hpp"
#include "utils/config.hpp"
#include "utils/mutex.hpp"
#include "utils/stdint.hpp"
#include "core/options.hpp"
#include "utils/atomic_counter.hpp"
#include "utils/condition_variable.hpp"

namespace zlink
{
class object_t;
class io_thread_t;
class socket_base_t;
class reaper_t;
class pipe_t;
class control_runtime_t;
class ctx_termination_test_access_t;

//  Context object encapsulates all the global state associated with
//  the library.

class ctx_t ZLINK_FINAL
{
  public:
    //  Create the context object.
    ctx_t ();

    //  Returns false if object is not a context.
    bool check_tag () const;

    //  This function is called when user invokes zlink_ctx_term. If there are
    //  no more sockets open it'll cause all the infrastructure to be shut
    //  down. If there are open sockets still, the deallocation happens
    //  after the last one is closed.
    int terminate ();

    // This function starts the terminate process by unblocking any blocking
    // operations currently in progress and stopping any more socket activity
    // (except zlink_close).
    // This function is non-blocking.
    // terminate must still be called afterwards.
    // This function is optional, terminate will unblock any current
    // operations as well.
    int shutdown ();

    //  Set and get context properties.
    int set (int option_, const void *optval_, size_t optvallen_);
    int get (int option_, void *optval_, const size_t *optvallen_);
    int get (int option_);

    //  Create and destroy a socket.
    zlink::socket_base_t *create_socket (int type_);
    void destroy_socket (zlink::socket_base_t *socket_);
    int wait_for_socket_removal (const zlink::socket_base_t *socket_, int timeout_ms_);
    int close_socket_and_wait (zlink::socket_base_t *&socket_, int timeout_ms_);
    size_t socket_count () const;
    int wait_for_socket_count_at_most (size_t max_count_, int timeout_ms_);

    //  Send command to the destination thread.
    void send_command (uint32_t tid_, const command_t &command_);

    //  Returns the I/O thread that is the least busy at the moment.
    //  Affinity specifies which I/O threads are eligible (0 = all).
    //  Returns NULL if no I/O thread is available.
    zlink::io_thread_t *choose_io_thread (uint64_t affinity_);

    //  STREAM-specific I/O thread selection policy.
    //  Selection is controlled by ZLINK_ASIO_STREAM_SESSION_SCHED:
    //   - rr (default): round-robin across eligible threads
    //   - minload: choose the least loaded eligible thread
    zlink::io_thread_t *choose_io_thread_stream (uint64_t affinity_);

    //  Returns reaper thread object.
    zlink::object_t *get_reaper () const;

    //  Management of inproc endpoints.
    int register_endpoint (const char *addr_, const endpoint_t &endpoint_);
    int unregister_endpoint (const std::string &addr_, const socket_base_t *socket_);
    void unregister_endpoints (const zlink::socket_base_t *socket_);
    endpoint_t find_endpoint (const char *addr_);
    bool pend_connection (const std::string &addr_, const endpoint_t &endpoint_, pipe_t **pipes_);
    void connect_pending (const char *addr_, zlink::socket_base_t *bind_socket_);
    int materialize_pending_inproc (const std::string &addr_, socket_base_t *connect_socket_);


    enum
    {
        term_tid = 0,
        reaper_tid = 1
    };

    ~ctx_t ();

    bool valid () const;
    control_runtime_t *control_runtime ();
    void
    start_thread (thread_t &thread_, thread_fn *tfn_, void *arg_, const char *name_ = NULL) const;
    const thread_ctx_t &thread_context () const;
    void schedule_auto_hwm_recalculate ();
    int auto_hwm_recalculate_now ();
    zlink_auto_hwm_profile_t auto_hwm_profile () const;
    bool auto_hwm_enabled () const;
    uint64_t auto_hwm_msg_unit_bytes () const;

  private:
    friend class ctx_termination_test_access_t;

    bool start ();
    bool start_runtime_locked ();
    control_runtime_t *ensure_control_runtime ();
    void teardown_runtime ();
    void flush_pending_inproc_locked ();
    bool begin_shutdown_locked (bool allow_fork_cleanup_);
    int wait_for_reaper_done ();
    static int clipped_maxsocket (int max_requested_);
    void debug_dump_sockets_locked (const char *phase_) const;
    static void auto_hwm_recalc_task_main (void *arg_);
    void auto_hwm_recalc_task ();
    void ensure_auto_hwm_recalc_task_started ();
    void stop_auto_hwm_recalc_task ();

    //  Used to check whether the object is a context.
    uint32_t _tag;

    //  If true, zlink_ctx_new has been called but no socket has been created
    //  yet. Launching of I/O threads is delayed.
    bool _starting;

    //  If true, zlink_ctx_term was already called.
    bool _terminating;

    //  Synchronisation of accesses to global slot-related data:
    //  sockets, empty_slots, terminating. It also synchronises
    //  access to zombie sockets as such (as opposed to slots) and provides
    //  a memory barrier to ensure that all CPU cores see the same data.
    mutex_t _slot_sync;
    ctx_socket_registry_t _socket_registry;

    //  Mailbox for zlink_ctx_term thread.
    mailbox_t _term_mailbox;

    ctx_inproc_registry_t _inproc_registry;
    ctx_runtime_resources_t _runtime_resources;
    thread_ctx_t _thread_context;

    //  Synchronisation of access to context options.
    mutable mutex_t _opt_sync;

    //  Maximum socket ID.
    static atomic_counter_t max_socket_id;

    //  Maximum number of sockets that can be opened at the same time.
    int _max_sockets;

    //  Maximum allowed message size
    int _max_msgsz;

    //  Number of I/O threads to launch.
    int _io_thread_count;

    ctx_auto_hwm_state_t _auto_hwm;

    //  Does context wait (possibly forever) on termination?
    bool _blocky;

    //  Is IPv6 enabled on this context?
    bool _ipv6;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (ctx_t)

#ifdef HAVE_FORK
    // the process that created this context. Used to detect forking.
    pid_t _pid;
#endif
};
}

#endif
