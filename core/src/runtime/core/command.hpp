/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_COMMAND_HPP_INCLUDED__
#define __ZLINK_COMMAND_HPP_INCLUDED__

#include <string>
#include "utils/stdint.hpp"
#include "core/endpoint.hpp"
#include "platform.hpp"

namespace zlink
{
class object_t;
class own_t;
struct i_engine;
class pipe_t;
class socket_base_t;
class session_base_t;

//  This structure defines the commands that can be sent between threads.

struct command_t
{
    //  Object to process the command.
    zlink::object_t *destination;

    enum type_t
    {
        stop,
        plug,
        own,
        attach,
        bind,
        activate_read,
        activate_write,
        flow_state,
        peer_weight,
        request_completion,
        transport_pair_owner_request,
        transport_pair_owner_decision,
        hiccup,
        pipe_term,
        pipe_term_ack,
        pipe_hwm,
        term_req,
        term,
        term_ack,
        term_endpoint,
        reconnect_inproc,
        reap,
        reaped,
        inproc_connected,
        conn_failed,
        done
    } type;

    union args_t
    {
        //  Sent to I/O thread to let it know that it should
        //  terminate itself.
        struct
        {
        } stop;

        //  Sent to I/O object to make it register with its I/O thread.
        struct
        {
        } plug;

        //  Sent to socket to let it know about the newly created object.
        struct
        {
            zlink::own_t *object;
        } own;

        //  Attach the engine to the session. If engine is NULL, it informs
        //  session that the connection have failed.
        struct
        {
            struct i_engine *engine;
        } attach;

        //  Sent from session to socket to establish pipe(s) between them.
        //  Caller have used inc_seqnum beforehand sending the command.
        struct
        {
            zlink::pipe_t *pipe;
        } bind;

        //  Sent by pipe writer to inform dormant pipe reader that there
        //  are messages in the pipe.
        struct
        {
        } activate_read;

        //  Sent by pipe reader to return cumulative byte and message credit.
        struct
        {
            uint64_t generation;
            uint64_t msgs_read;
            uint64_t bytes_read;
        } activate_write;

        //  Returns one retained receive frame's exact origin credit on the
        //  reader pipe's owning thread.
        //  Applies the peer's absolute receive-flow state to one application
        //  pipe on the socket thread. The state is not a counter, so a repeated
        //  value is simply idempotent.
        struct
        {
            //  Ordering tag of the state, not a count. A command whose epoch
            //  does not advance is a stale replay and is ignored, which is what
            //  keeps an attach-time replay from overwriting a newer state.
            uint64_t epoch;
            unsigned char state;
        } flow_state;

        //  Delivers peer scheduling policy to one socket-owned pipe endpoint.
        //  Inproc sends it directly; a network session queues it after
        //  consuming the corresponding ZMP command frame.
        struct
        {
            uint64_t connection_id;
            uint32_t weight;
        } peer_weight;

        //  Schedules completion-pipe/control processing on the socket mailbox
        //  owner. The command carries no payload; the socket-owned queues are
        //  the authoritative state.
        struct
        {
        } request_completion;

        //  Asks the socket mailbox owner to decide the HELLO-derived physical
        //  lane topology and, when required, create the Completion child.
        struct
        {
            zlink::session_base_t *session;
            int peer_socket_type;
            uint64_t connection_id;
            uint64_t pair_id;
            uint64_t generation;
            unsigned char lane;
        } transport_pair_owner_request;

        //  Returns the socket-owner decision to the requesting session. The
        //  session pre-reserves this command's seqnum before sending the request.
        struct
        {
            uint64_t connection_id;
            uint64_t pair_id;
            uint64_t generation;
            unsigned char lane_count;
            int error_number;
        } transport_pair_owner_decision;

        //  Sent by pipe reader to writer after creating a new inpipe.
        //  The parameter is actually of type pipe_t::upipe_t, however,
        //  its definition is private so we'll have to do with void*.
        struct
        {
            void *pipe;
            uint64_t generation;
        } hiccup;

        //  Sent by pipe reader to pipe writer to ask it to terminate
        //  its end of the pipe.
        struct
        {
        } pipe_term;

        //  Pipe writer acknowledges pipe_term command.
        struct
        {
        } pipe_term_ack;

        //  Sent by one of pipe to another part for modify hwm
        struct
        {
            uint64_t inhwm;
            uint64_t outhwm;
        } pipe_hwm;

        //  Sent by I/O object ot the socket to request the shutdown of
        //  the I/O object.
        struct
        {
            zlink::own_t *object;
        } term_req;

        //  Sent by socket to I/O object to start its shutdown.
        struct
        {
            int linger;
        } term;

        //  Sent by I/O object to the socket to acknowledge it has
        //  shut down.
        struct
        {
        } term_ack;

        //  Sent by session_base (I/O thread) to socket (application thread)
        //  to ask to disconnect the endpoint.
        struct
        {
            std::string *endpoint;
        } term_endpoint;

        //  Sent by an inproc connector pipe after an unexpected peer detach.
        struct
        {
            std::string *endpoint;
        } reconnect_inproc;

        //  Transfers the ownership of the closed socket
        //  to the reaper thread.
        struct
        {
            zlink::socket_base_t *socket;
        } reap;

        //  Closed socket notifies the reaper that it's already deallocated.
        struct
        {
        } reaped;

        //  Sent by reaper thread to the term thread when all the sockets
        //  are successfully deallocated.
        struct
        {
        } done;

    } args;
#ifdef _MSC_VER
};
#else
}
#ifdef HAVE_POSIX_MEMALIGN
__attribute__ ((aligned (ZLINK_CACHELINE_SIZE)))
#endif
;
#endif
}

#endif
