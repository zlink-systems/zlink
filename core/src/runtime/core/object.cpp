/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include <string.h>
#include <stdarg.h>

#include "core/object.hpp"
#include "core/ctx.hpp"
#include "utils/err.hpp"
#include "core/pipe.hpp"
#include "core/io_thread.hpp"
#include "core/session_base.hpp"
#include "sockets/common/socket_base.hpp"

namespace
{
zlink::pipe_t *pipe_command_destination (const zlink::command_t &cmd_)
{
    switch (cmd_.type) {
        case zlink::command_t::bind:
            return cmd_.args.bind.pipe;
        case zlink::command_t::activate_read:
        case zlink::command_t::activate_write:
        case zlink::command_t::hiccup:
        case zlink::command_t::pipe_term:
        case zlink::command_t::pipe_term_ack:
        case zlink::command_t::pipe_hwm:
            return static_cast<zlink::pipe_t *> (cmd_.destination);
        default:
            return NULL;
    }
}
}

zlink::object_t::object_t (ctx_t *ctx_, uint32_t tid_) : _ctx (ctx_), _tid (tid_)
{
}

zlink::object_t::object_t (object_t *parent_) : _ctx (parent_->_ctx), _tid (parent_->_tid)
{
}

zlink::object_t::~object_t ()
{
}

uint32_t zlink::object_t::get_tid () const
{
    return _tid;
}

void zlink::object_t::set_tid (uint32_t id_)
{
    _tid = id_;
}

zlink::ctx_t *zlink::object_t::get_ctx () const
{
    return _ctx;
}

void zlink::object_t::process_command (const command_t &cmd_)
{
    switch (cmd_.type) {
        case command_t::activate_read:
            process_activate_read ();
            break;

        case command_t::activate_write:
            process_activate_write (cmd_.args.activate_write.msgs_read,
                                    cmd_.args.activate_write.bytes_read);
            break;

        case command_t::stop:
            process_stop ();
            break;

        case command_t::plug:
            process_plug ();
            process_seqnum ();
            break;

        case command_t::own:
            process_own (cmd_.args.own.object);
            process_seqnum ();
            break;

        case command_t::attach:
            process_attach (cmd_.args.attach.engine);
            process_seqnum ();
            break;

        case command_t::bind:
            process_bind (cmd_.args.bind.pipe);
            process_seqnum ();
            break;

        case command_t::hiccup:
            process_hiccup (cmd_.args.hiccup.pipe);
            break;

        case command_t::pipe_term:
            process_pipe_term ();
            break;

        case command_t::pipe_term_ack:
            process_pipe_term_ack ();
            break;

        case command_t::pipe_hwm:
            process_pipe_hwm (cmd_.args.pipe_hwm.inhwm, cmd_.args.pipe_hwm.outhwm);
            break;

        case command_t::term_req:
            process_term_req (cmd_.args.term_req.object);
            break;

        case command_t::term:
            process_term (cmd_.args.term.linger);
            break;

        case command_t::term_ack:
            process_term_ack ();
            break;

        case command_t::term_endpoint:
            process_term_endpoint (cmd_.args.term_endpoint.endpoint);
            break;

        case command_t::reap:
            process_reap (cmd_.args.reap.socket);
            break;

        case command_t::reaped:
            process_reaped ();
            break;

        case command_t::inproc_connected:
            process_seqnum ();
            break;

        case command_t::conn_failed:
            process_conn_failed ();
            break;

        case command_t::done:
        default:
            fprintf (stderr, "[object-cmd] unexpected command type=%d this=%p tid=%u dest=%p\n",
                     static_cast<int> (cmd_.type), static_cast<void *> (this),
                     static_cast<unsigned int> (_tid), static_cast<void *> (cmd_.destination));
            fflush (stderr);
            zlink_assert (false);
    }

    pipe_t *pipe = pipe_command_destination (cmd_);
    if (pipe)
        pipe->release_lifetime_ref ();
}

int zlink::object_t::register_endpoint (const char *addr_, const endpoint_t &endpoint_)
{
    return _ctx->register_endpoint (addr_, endpoint_);
}

int zlink::object_t::unregister_endpoint (const std::string &addr_, socket_base_t *socket_)
{
    return _ctx->unregister_endpoint (addr_, socket_);
}

void zlink::object_t::unregister_endpoints (socket_base_t *socket_)
{
    return _ctx->unregister_endpoints (socket_);
}

zlink::endpoint_t zlink::object_t::find_endpoint (const char *addr_) const
{
    return _ctx->find_endpoint (addr_);
}

bool zlink::object_t::pend_connection (const std::string &addr_,
                                       const endpoint_t &endpoint_,
                                       pipe_t **pipes_)
{
    return _ctx->pend_connection (addr_, endpoint_, pipes_);
}

void zlink::object_t::connect_pending (const char *addr_, zlink::socket_base_t *bind_socket_)
{
    return _ctx->connect_pending (addr_, bind_socket_);
}

void zlink::object_t::destroy_socket (socket_base_t *socket_)
{
    _ctx->destroy_socket (socket_);
}

zlink::io_thread_t *zlink::object_t::choose_io_thread (uint64_t affinity_) const
{
    return _ctx->choose_io_thread (affinity_);
}

zlink::io_thread_t *zlink::object_t::choose_io_thread_stream (uint64_t affinity_) const
{
    return _ctx->choose_io_thread_stream (affinity_);
}

void zlink::object_t::send_stop ()
{
    //  'stop' command goes always from administrative thread to
    //  the current object.
    command_t cmd;
    cmd.destination = this;
    cmd.type = command_t::stop;
    _ctx->send_command (_tid, cmd);
}

void zlink::object_t::send_plug (own_t *destination_, bool inc_seqnum_)
{
    if (inc_seqnum_)
        destination_->inc_seqnum ();

    command_t cmd;
    cmd.destination = destination_;
    cmd.type = command_t::plug;
    send_command (cmd);
}

void zlink::object_t::send_own (own_t *destination_, own_t *object_)
{
    destination_->inc_seqnum ();
    command_t cmd;
    cmd.destination = destination_;
    cmd.type = command_t::own;
    cmd.args.own.object = object_;
    send_command (cmd);
}

void zlink::object_t::send_attach (session_base_t *destination_,
                                   i_engine *engine_,
                                   bool inc_seqnum_)
{
    if (inc_seqnum_)
        destination_->inc_seqnum ();

    command_t cmd;
    cmd.destination = destination_;
    cmd.type = command_t::attach;
    cmd.args.attach.engine = engine_;
    send_command (cmd);
}

void zlink::object_t::send_conn_failed (session_base_t *destination_)
{
    command_t cmd;
    cmd.destination = destination_;
    cmd.type = command_t::conn_failed;
    send_command (cmd);
}

bool zlink::object_t::send_bind (own_t *destination_, pipe_t *pipe_, bool inc_seqnum_)
{
    if (!pipe_->retain_lifetime_ref ())
        return false;
    if (inc_seqnum_)
        destination_->inc_seqnum ();

    command_t cmd;
    cmd.destination = destination_;
    cmd.type = command_t::bind;
    cmd.args.bind.pipe = pipe_;
    send_command (cmd);
    return true;
}

void zlink::object_t::send_activate_read (pipe_t *destination_)
{
    command_t cmd;
    cmd.type = command_t::activate_read;
    send_pipe_command (destination_, cmd, false);
}

void zlink::object_t::send_activate_write (pipe_t *destination_,
                                           uint64_t msgs_read_,
                                           uint64_t bytes_read_)
{
    command_t cmd;
    cmd.type = command_t::activate_write;
    cmd.args.activate_write.msgs_read = msgs_read_;
    cmd.args.activate_write.bytes_read = bytes_read_;
    send_pipe_command (destination_, cmd, true);
}

void zlink::object_t::send_hiccup (pipe_t *destination_, void *pipe_)
{
    command_t cmd;
    cmd.type = command_t::hiccup;
    cmd.args.hiccup.pipe = pipe_;
    send_pipe_command (destination_, cmd, false);
}

void zlink::object_t::send_pipe_term (pipe_t *destination_)
{
    command_t cmd;
    cmd.type = command_t::pipe_term;
    send_pipe_command (destination_, cmd, false);
}

void zlink::object_t::send_pipe_term_ack (pipe_t *destination_)
{
    command_t cmd;
    cmd.type = command_t::pipe_term_ack;
    send_pipe_command (destination_, cmd, false);
}

void zlink::object_t::send_pipe_hwm (pipe_t *destination_,
                                     uint64_t inhwm_,
                                     uint64_t outhwm_)
{
    command_t cmd;
    cmd.type = command_t::pipe_hwm;
    cmd.args.pipe_hwm.inhwm = inhwm_;
    cmd.args.pipe_hwm.outhwm = outhwm_;
    send_pipe_command (destination_, cmd, false);
}

void zlink::object_t::send_term_req (own_t *destination_, own_t *object_)
{
    command_t cmd;
    cmd.destination = destination_;
    cmd.type = command_t::term_req;
    cmd.args.term_req.object = object_;
    send_command (cmd);
}

void zlink::object_t::send_term (own_t *destination_, int linger_)
{
    command_t cmd;
    cmd.destination = destination_;
    cmd.type = command_t::term;
    cmd.args.term.linger = linger_;
    send_command (cmd);
}

void zlink::object_t::send_term_ack (own_t *destination_)
{
    command_t cmd;
    cmd.destination = destination_;
    cmd.type = command_t::term_ack;
    send_command (cmd);
}

void zlink::object_t::send_term_endpoint (own_t *destination_, std::string *endpoint_)
{
    command_t cmd;
    cmd.destination = destination_;
    cmd.type = command_t::term_endpoint;
    cmd.args.term_endpoint.endpoint = endpoint_;
    send_command (cmd);
}

void zlink::object_t::send_reap (class socket_base_t *socket_)
{
    command_t cmd;
    cmd.destination = _ctx->get_reaper ();
    cmd.type = command_t::reap;
    cmd.args.reap.socket = socket_;
    send_command (cmd);
}

void zlink::object_t::send_reaped ()
{
    command_t cmd;
    cmd.destination = _ctx->get_reaper ();
    cmd.type = command_t::reaped;
    send_command (cmd);
}

void zlink::object_t::send_inproc_connected (zlink::socket_base_t *socket_)
{
    command_t cmd;
    cmd.destination = socket_;
    cmd.type = command_t::inproc_connected;
    send_command (cmd);
}

void zlink::object_t::send_done ()
{
    command_t cmd;
    cmd.destination = NULL;
    cmd.type = command_t::done;
    _ctx->send_command (ctx_t::term_tid, cmd);
}

void zlink::object_t::process_stop ()
{
    zlink_assert (false);
}

void zlink::object_t::process_plug ()
{
    zlink_assert (false);
}

void zlink::object_t::process_own (own_t *)
{
    zlink_assert (false);
}

void zlink::object_t::process_attach (i_engine *)
{
    zlink_assert (false);
}

void zlink::object_t::process_bind (pipe_t *)
{
    zlink_assert (false);
}

void zlink::object_t::process_activate_read ()
{
    zlink_assert (false);
}

void zlink::object_t::process_activate_write (uint64_t, uint64_t)
{
    zlink_assert (false);
}

void zlink::object_t::process_hiccup (void *)
{
    zlink_assert (false);
}

void zlink::object_t::process_pipe_term ()
{
    zlink_assert (false);
}

void zlink::object_t::process_pipe_term_ack ()
{
    zlink_assert (false);
}

void zlink::object_t::process_pipe_hwm (uint64_t, uint64_t)
{
    zlink_assert (false);
}

void zlink::object_t::process_term_req (own_t *)
{
    zlink_assert (false);
}

void zlink::object_t::process_term (int)
{
    zlink_assert (false);
}

void zlink::object_t::process_term_ack ()
{
    zlink_assert (false);
}

void zlink::object_t::process_term_endpoint (std::string *)
{
    zlink_assert (false);
}

void zlink::object_t::process_reap (class socket_base_t *)
{
    zlink_assert (false);
}

void zlink::object_t::process_reaped ()
{
    zlink_assert (false);
}

void zlink::object_t::process_seqnum ()
{
    zlink_assert (false);
}

void zlink::object_t::process_conn_failed ()
{
    zlink_assert (false);
}

void zlink::object_t::send_pipe_command (pipe_t *destination_,
                                         command_t &cmd_,
                                         bool allow_self_dispatch_)
{
    cmd_.destination = destination_;
    if (!destination_->retain_lifetime_ref ())
        return;
    if (allow_self_dispatch_ && destination_->get_tid () == _tid)
        destination_->process_command (cmd_);
    else
        send_command (cmd_);
}

void zlink::object_t::send_command (const command_t &cmd_)
{
    _ctx->send_command (cmd_.destination->get_tid (), cmd_);
}
