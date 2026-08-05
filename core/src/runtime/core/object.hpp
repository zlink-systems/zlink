/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_OBJECT_HPP_INCLUDED__
#define __ZLINK_OBJECT_HPP_INCLUDED__

#include <string>

#include "core/endpoint.hpp"
#include "utils/macros.hpp"
#include "utils/stdint.hpp"

namespace zlink
{
struct i_engine;
struct endpoint_t;
struct command_t;
class ctx_t;
class pipe_t;
class socket_base_t;
class session_base_t;
class io_thread_t;
class own_t;

//  Base class for all objects that participate in inter-thread
//  communication.

class object_t
{
  public:
    object_t (zlink::ctx_t *ctx_, uint32_t tid_);
    object_t (object_t *parent_);
    virtual ~object_t ();

    uint32_t get_tid () const;
    void set_tid (uint32_t id_);
    ctx_t *get_ctx () const;
    void process_command (const zlink::command_t &cmd_);
    void send_inproc_connected (zlink::socket_base_t *socket_);
    bool send_bind (zlink::own_t *destination_, zlink::pipe_t *pipe_, bool inc_seqnum_ = true);

  protected:
    //  Using following function, socket is able to access global
    //  repository of inproc endpoints.
    int register_endpoint (const char *addr_, const zlink::endpoint_t &endpoint_);
    int unregister_endpoint (const std::string &addr_, socket_base_t *socket_);
    void unregister_endpoints (zlink::socket_base_t *socket_);
    zlink::endpoint_t find_endpoint (const char *addr_) const;
    bool pend_connection (const std::string &addr_, const endpoint_t &endpoint_, pipe_t **pipes_);
    void connect_pending (const char *addr_, zlink::socket_base_t *bind_socket_);

    void destroy_socket (zlink::socket_base_t *socket_);

    //  Logs an message.
    void log (const char *format_, ...);

    //  Chooses least loaded I/O thread.
    zlink::io_thread_t *choose_io_thread (uint64_t affinity_) const;

    //  Chooses I/O thread using STREAM-specific policy.
    zlink::io_thread_t *choose_io_thread_stream (uint64_t affinity_) const;

    //  Derived object can use these functions to send commands
    //  to other objects.
    void send_stop ();
    void send_plug (zlink::own_t *destination_, bool inc_seqnum_ = true);
    void send_own (zlink::own_t *destination_, zlink::own_t *object_);
    void send_attach (zlink::session_base_t *destination_,
                      zlink::i_engine *engine_,
                      bool inc_seqnum_ = true);
    void send_activate_read (zlink::pipe_t *destination_);
    void send_activate_write (zlink::pipe_t *destination_,
                              uint64_t msgs_read_,
                              uint64_t bytes_read_);
    void send_hiccup (zlink::pipe_t *destination_, void *pipe_);
    void send_pipe_term (zlink::pipe_t *destination_);
    void send_pipe_term_ack (zlink::pipe_t *destination_);
    void send_pipe_hwm (zlink::pipe_t *destination_, uint64_t inhwm_, uint64_t outhwm_);
    void send_term_req (zlink::own_t *destination_, zlink::own_t *object_);
    void send_term (zlink::own_t *destination_, int linger_);
    void send_term_ack (zlink::own_t *destination_);
    void send_term_endpoint (own_t *destination_, std::string *endpoint_);
    void send_reap (zlink::socket_base_t *socket_);
    void send_reaped ();
    void send_done ();
    void send_conn_failed (zlink::session_base_t *destination_);


    //  These handlers can be overridden by the derived objects. They are
    //  called when command arrives from another thread.
    virtual void process_stop ();
    virtual void process_plug ();
    virtual void process_own (zlink::own_t *object_);
    virtual void process_attach (zlink::i_engine *engine_);
    virtual void process_bind (zlink::pipe_t *pipe_);
    virtual void process_activate_read ();
    virtual void process_activate_write (uint64_t msgs_read_, uint64_t bytes_read_);
    virtual void process_hiccup (void *pipe_);
    virtual void process_pipe_term ();
    virtual void process_pipe_term_ack ();
    virtual void process_pipe_hwm (uint64_t inhwm_, uint64_t outhwm_);
    virtual void process_term_req (zlink::own_t *object_);
    virtual void process_term (int linger_);
    virtual void process_term_ack ();
    virtual void process_term_endpoint (std::string *endpoint_);
    virtual void process_reap (zlink::socket_base_t *socket_);
    virtual void process_reaped ();
    virtual void process_conn_failed ();


    //  Special handler called after a command that requires a seqnum
    //  was processed. The implementation should catch up with its counter
    //  of processed commands here.
    virtual void process_seqnum ();

  private:
    //  Context provides access to the global state.
    zlink::ctx_t *const _ctx;

    //  Thread ID of the thread the object belongs to.
    uint32_t _tid;

    void send_command (const command_t &cmd_);
    void send_pipe_command (zlink::pipe_t *destination_,
                            command_t &cmd_,
                            bool allow_self_dispatch_);

    ZLINK_NON_COPYABLE_NOR_MOVABLE (object_t)
};
}

#endif
