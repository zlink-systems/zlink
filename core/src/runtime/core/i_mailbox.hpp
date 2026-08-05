/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_I_MAILBOX_HPP_INCLUDED__
#define __ZLINK_I_MAILBOX_HPP_INCLUDED__

#include "utils/macros.hpp"
#include "utils/stdint.hpp"

namespace zlink
{
//  Interface to be implemented by mailbox.

class i_mailbox
{
  public:
    virtual ~i_mailbox () ZLINK_DEFAULT;

    virtual void send (const command_t &cmd_) = 0;
    virtual int recv (command_t *cmd_, int timeout_) = 0;


#ifdef HAVE_FORK
    // close the file descriptors in the signaller. This is used in a forked
    // child process to close the file descriptors so that they do not interfere
    // with the context in the parent process.
    virtual void forked () = 0;
#endif
};
}

#endif
