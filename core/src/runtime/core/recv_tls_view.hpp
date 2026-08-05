/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CORE_RECV_TLS_VIEW_HPP_INCLUDED__
#define __ZLINK_CORE_RECV_TLS_VIEW_HPP_INCLUDED__

#include <vector>

#include "core/msg.hpp"
#include "utils/env.hpp"
#include "zlink.h"

namespace zlink
{
namespace recv_tls_view
{
struct storage_t
{
    storage_t () : count (0), initialized (false) {}

    size_t count;
    bool initialized;
    std::vector<zlink_msg_t> parts;
    std::vector<unsigned char> occupied;
};

inline size_t payload_cap ()
{
    static const size_t cap = [] () -> size_t {
        return static_cast<size_t> (env::positive_int ("ZLINK_RECV_TLS_PAYLOAD_CAP", 2));
    }();
    return cap;
}

inline storage_t &storage ()
{
    static thread_local storage_t tls;
    if (!tls.initialized) {
        const size_t cap = payload_cap ();
        tls.parts.resize (cap);
        tls.occupied.assign (cap, 0);
        for (size_t i = 0; i < cap; ++i)
            (void) zlink_msg_init (&tls.parts[i]);
        tls.initialized = true;
    }
    return tls;
}

inline void reset ()
{
    storage_t &tls = storage ();
    for (size_t i = 0; i < tls.count; ++i) {
        if (!tls.occupied[i])
            continue;

        if (reinterpret_cast<zlink::msg_t *> (&tls.parts[i])->check ())
            (void) zlink_msg_close (&tls.parts[i]);
        (void) zlink_msg_init (&tls.parts[i]);
        tls.occupied[i] = 0;
    }
    tls.count = 0;
}

inline bool owns_prefix (zlink_msg_t *parts_, size_t part_count_)
{
    if (!parts_)
        return false;

    storage_t &tls = storage ();
    if (tls.parts.empty () || part_count_ == 0 || part_count_ > tls.count)
        return false;

    return parts_ == &tls.parts[0];
}

inline bool release_closed_prefix (zlink_msg_t *parts_, size_t part_count_)
{
    if (!owns_prefix (parts_, part_count_))
        return false;

    storage_t &tls = storage ();
    for (size_t i = 0; i < part_count_; ++i) {
        if (!tls.occupied[i])
            continue;

        if (reinterpret_cast<zlink::msg_t *> (&tls.parts[i])->check ())
            (void) zlink_msg_close (&tls.parts[i]);
        (void) zlink_msg_init (&tls.parts[i]);
        tls.occupied[i] = 0;
    }

    if (part_count_ == tls.count)
        tls.count = 0;

    return true;
}

inline int begin (zlink_msg_t **parts_out_, size_t *part_count_out_)
{
    if (!parts_out_ || !part_count_out_) {
        errno = EFAULT;
        return -1;
    }

    reset ();
    *parts_out_ = NULL;
    *part_count_out_ = 0;
    return 0;
}

inline int begin_with_first_slot (zlink_msg_t **parts_out_,
                                  size_t *part_count_out_,
                                  zlink_msg_t **first_slot_out_)
{
    if (!parts_out_ || !part_count_out_ || !first_slot_out_) {
        errno = EFAULT;
        return -1;
    }

    reset ();
    *parts_out_ = NULL;
    *part_count_out_ = 0;

    storage_t &tls = storage ();
    if (tls.parts.empty ()) {
        errno = EMSGSIZE;
        return -1;
    }

    *first_slot_out_ = &tls.parts[0];
    return 0;
}

inline void abort ()
{
    reset ();
}

inline int push (zlink_msg_t *src_)
{
    if (!src_) {
        errno = EFAULT;
        return -1;
    }

    storage_t &tls = storage ();
    if (tls.count >= tls.parts.size ()) {
        const size_t old_size = tls.parts.size ();
        const size_t new_size = old_size == 0 ? 1 : old_size * 2;
        tls.parts.resize (new_size);
        tls.occupied.resize (new_size, 0);
        for (size_t i = old_size; i < new_size; ++i)
            (void) zlink_msg_init (&tls.parts[i]);
    }

    if (zlink_msg_move (&tls.parts[tls.count], src_) != 0)
        return -1;

    tls.occupied[tls.count] = 1;
    ++tls.count;
    return 0;
}

inline int export_single (zlink_msg_t *src_, zlink_msg_t **parts_out_, size_t *part_count_out_)
{
    if (!src_ || !parts_out_ || !part_count_out_) {
        errno = EFAULT;
        return -1;
    }

    storage_t &tls = storage ();
    if (tls.parts.empty ()) {
        errno = EMSGSIZE;
        return -1;
    }

    if (zlink_msg_move (&tls.parts[0], src_) != 0)
        return -1;

    tls.occupied[0] = 1;
    tls.count = 1;
    *parts_out_ = &tls.parts[0];
    *part_count_out_ = 1;
    errno = 0;
    return 0;
}

inline int commit (zlink_msg_t **parts_out_, size_t *part_count_out_)
{
    if (!parts_out_ || !part_count_out_) {
        errno = EFAULT;
        return -1;
    }

    storage_t &tls = storage ();
    *parts_out_ = tls.count > 0 ? &tls.parts[0] : NULL;
    *part_count_out_ = tls.count;
    errno = 0;
    return 0;
}

inline int reserve_first_slot ()
{
    storage_t &tls = storage ();
    if (tls.parts.empty ()) {
        errno = EMSGSIZE;
        return -1;
    }

    tls.occupied[0] = 1;
    tls.count = 1;
    return 0;
}

inline int commit_reserved_single (zlink_msg_t **parts_out_, size_t *part_count_out_)
{
    if (reserve_first_slot () != 0)
        return -1;

    return commit (parts_out_, part_count_out_);
}
}
}

#endif
