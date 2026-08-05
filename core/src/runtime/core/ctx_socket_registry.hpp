/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CTX_SOCKET_REGISTRY_HPP_INCLUDED__
#define __ZLINK_CTX_SOCKET_REGISTRY_HPP_INCLUDED__

#include <vector>

#include "core/command.hpp"
#include "core/i_mailbox.hpp"
#include "utils/array.hpp"
#include "utils/condition_variable.hpp"
#include "utils/mutex.hpp"

namespace zlink
{
class socket_base_t;

class ctx_socket_registry_t
{
  public:
    typedef array_t<socket_base_t> sockets_t;

    bool
    initialize_slot_pool (size_t slot_count_, size_t first_socket_slot_, i_mailbox *term_mailbox_);
    void clear ();

    void bind_mailbox (uint32_t tid_, i_mailbox *mailbox_);
    i_mailbox *mailbox (uint32_t tid_) const;

    bool has_available_socket_slot () const;
    uint32_t claim_socket_slot ();
    void release_unused_socket_slot (uint32_t slot_);

    void publish_socket (socket_base_t *socket_);
    void remove_socket (socket_base_t *socket_);
    bool empty () const;
    size_t socket_count () const;

    void collect_sockets (std::vector<socket_base_t *> *out_) const;
    int wait_for_socket_removal (mutex_t *sync_, const socket_base_t *socket_, int timeout_ms_);
    int wait_for_socket_count_at_most (mutex_t *sync_, size_t max_count_, int timeout_ms_);

  private:
    bool contains_socket (const socket_base_t *socket_) const;

    typedef std::vector<uint32_t> empty_slots_t;
    typedef std::vector<i_mailbox *> slots_t;

    sockets_t _sockets;
    empty_slots_t _empty_slots;
    slots_t _slots;
    condition_variable_t _socket_state_cv;
};
}

#endif
