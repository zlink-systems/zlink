/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_I_ENGINE_HPP_INCLUDED__
#define __ZLINK_I_ENGINE_HPP_INCLUDED__

#include "core/endpoint.hpp"
#include "utils/macros.hpp"

namespace zlink
{
class io_thread_t;

//  Abstract interface to be implemented by various engines.

struct i_engine
{
    enum error_reason_t
    {
        protocol_error,
        connection_error,
        timeout_error
    };

    virtual ~i_engine () ZLINK_DEFAULT;

    //  Indicate if the engine has an handshake stage.
    //  If engine has handshake stage, engine must call session.engine_ready when the handshake is complete.
    virtual bool has_handshake_stage () = 0;

    //  Plug the engine to the session.
    virtual void plug (zlink::io_thread_t *io_thread_, class session_base_t *session_) = 0;

    //  Terminate and deallocate the engine. Note that 'detached'
    //  events are not fired on termination.
    virtual void terminate () = 0;

    //  This method is called by the session to signalise that more
    //  messages can be written to the pipe.
    //  Returns false if the engine reached its terminal heap-owner release path
    //  due to an error.
    virtual bool restart_input () = 0;

    //  This method is called by the session to signalise that there
    //  are messages to send available.
    virtual void restart_output () = 0;

    virtual const endpoint_uri_pair_t &get_endpoint () const = 0;

    //  ZMP active DEALER/ROUTER sessions defer READY until their socket owner
    //  has fixed the endpoint's lane topology. Other engines ignore this hook.
    virtual void transport_lane_count_decided (unsigned char lane_count_,
                                               int error_number_)
    {
        LIBZLINK_UNUSED (lane_count_);
        LIBZLINK_UNUSED (error_number_);
    }
};
}

#endif
