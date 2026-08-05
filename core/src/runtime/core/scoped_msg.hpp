/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CORE_SCOPED_MSG_HPP_INCLUDED__
#define __ZLINK_CORE_SCOPED_MSG_HPP_INCLUDED__

#include <zlink.h>

#include "utils/err.hpp"

namespace zlink
{

class scoped_msg_t
{
  public:
    scoped_msg_t () : _active (true)
    {
        const int rc = zlink_msg_init (&_msg);
        errno_assert (rc == 0);
    }

    ~scoped_msg_t () { close (); }

    scoped_msg_t (const scoped_msg_t &) = delete;
    scoped_msg_t &operator= (const scoped_msg_t &) = delete;

    zlink_msg_t *get () { return &_msg; }

    const zlink_msg_t *get () const { return &_msg; }

    zlink_msg_t &ref () { return _msg; }

    const zlink_msg_t &ref () const { return _msg; }

    void close ()
    {
        if (!_active)
            return;

        const int rc = zlink_msg_close (&_msg);
        errno_assert (rc == 0);
        _active = false;
    }

  private:
    zlink_msg_t _msg;
    bool _active;
};

}

#endif
