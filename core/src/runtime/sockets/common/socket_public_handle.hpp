/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SOCKET_PUBLIC_HANDLE_HPP_INCLUDED__
#define __ZLINK_SOCKET_PUBLIC_HANDLE_HPP_INCLUDED__

#include <atomic>
#include <stdint.h>

namespace zlink
{
class socket_base_t;

// Stable storage behind the opaque public socket handle.  The context owns
// this object; it deliberately does not own the socket.  A pin is acquired
// before the socket pointer (or its tag) is read, and socket destruction is
// deferred until the final pin leaves.
class socket_public_handle_t
{
  public:
    explicit socket_public_handle_t (socket_base_t *socket_);

    bool check_tag () const;
    bool acquire (socket_base_t **socket_out_);
    void add_ref ();
    void release ();

    bool begin_close ();
    void cancel_close ();

    // Called only after the socket's ordinary mailbox lifetime refs drain.
    // Returns true to the unique caller that must perform final destruction.
    bool request_destroy ();
    void clear_socket ();

  private:
    socket_public_handle_t (const socket_public_handle_t &);
    socket_public_handle_t &operator= (const socket_public_handle_t &);

    bool try_claim_final_destroy (uint32_t state_);

    static const uint32_t tag_value = 0x736f6368u;
    static const uint32_t closing_bit = 0x80000000u;
    static const uint32_t destroy_pending_bit = 0x40000000u;
    static const uint32_t finalizing_bit = 0x20000000u;
    static const uint32_t ref_mask = ~(closing_bit | destroy_pending_bit | finalizing_bit);

    const uint32_t _tag;
    std::atomic<uint32_t> _state;
    std::atomic<socket_base_t *> _socket;
};
}

#endif
