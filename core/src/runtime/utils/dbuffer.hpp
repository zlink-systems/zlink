/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_DBUFFER_HPP_INCLUDED__
#define __ZLINK_DBUFFER_HPP_INCLUDED__

#include <stdlib.h>
#include <stddef.h>

#include "utils/mutex.hpp"
#include "core/msg.hpp"
#include "core/ypipe_base.hpp"

namespace zlink
{
//  dbuffer is a single-producer single-consumer double-buffer
//  implementation.
//
//  The producer writes to a back buffer and then tries to swap
//  pointers between the back and front buffers. If it fails,
//  due to the consumer reading from the front buffer, it just
//  gives up, which is ok since writes are many and redundant.
//
//  The reader simply reads from the front buffer.
//
//  has_msg keeps track of whether there has been a not yet read
//  value written, it is used by ypipe_conflate to mimic ypipe
//  functionality regarding a reader being asleep

template <typename T> class dbuffer_t;

template <> class dbuffer_t<msg_t>
{
  public:
    dbuffer_t () :
        _back (&_storage[0]),
        _front (&_storage[1]),
        _has_msg (false),
        _has_back_msg (false)
    {
        _back->init ();
        _front->init ();
    }

    ~dbuffer_t ()
    {
        _back->close ();
        _front->close ();
    }

    void write (const msg_t &value_)
    {
        uint64_t ignored_bytes = 0;
        uint64_t ignored_messages = 0;
        write_with_replacement_accounting (
          value_, NULL, NULL, &ignored_bytes, &ignored_messages);
    }

    void write_with_replacement_accounting (
      const msg_t &value_,
      uint64_t (*accounted_bytes_) (const msg_t &),
      bool (*counted_message_) (const msg_t &),
      uint64_t *replaced_bytes_,
      uint64_t *replaced_messages_)
    {
        zlink_assert (value_.check ());
        zlink_assert (replaced_bytes_);
        zlink_assert (replaced_messages_);
        *replaced_bytes_ = 0;
        *replaced_messages_ = 0;

        if (_has_back_msg) {
            if (accounted_bytes_)
                *replaced_bytes_ = accounted_bytes_ (*_back);
            if (counted_message_ && counted_message_ (*_back))
                *replaced_messages_ = 1;
            const int rc = _back->close ();
            zlink_assert (rc == 0);
            _back->init ();
        }
        *_back = value_;
        _has_back_msg = true;

        zlink_assert (_back->check ());

        if (_sync.try_lock ()) {
            if (_has_msg) {
                if (accounted_bytes_) {
                    const uint64_t front_bytes = accounted_bytes_ (*_front);
                    *replaced_bytes_ =
                      UINT64_MAX - *replaced_bytes_ < front_bytes
                        ? UINT64_MAX
                        : *replaced_bytes_ + front_bytes;
                }
                if (counted_message_ && counted_message_ (*_front))
                    ++*replaced_messages_;
            }
            _front->move (*_back);
            _has_back_msg = false;
            _has_msg = true;

            _sync.unlock ();
        }
    }

    bool read (msg_t *value_)
    {
        if (!value_)
            return false;

        {
            scoped_lock_t lock (_sync);
            if (!_has_msg)
                return false;

            zlink_assert (_front->check ());

            *value_ = *_front;
            _front->init (); // avoid double free

            _has_msg = false;
            return true;
        }
    }


    bool check_read ()
    {
        scoped_lock_t lock (_sync);

        return _has_msg;
    }

    bool probe (bool (*fn_) (const msg_t &))
    {
        scoped_lock_t lock (_sync);
        return (*fn_) (*_front);
    }

    ypipe_read_result_t
    read_if (msg_t *value_, bool (*fn_) (const msg_t &, void *), void *userdata_)
    {
        if (!value_)
            return ypipe_read_rejected;

        scoped_lock_t lock (_sync);
        if (!_has_msg)
            return ypipe_read_empty;
        if (!(*fn_) (*_front, userdata_))
            return ypipe_read_rejected;

        zlink_assert (_front->check ());
        *value_ = *_front;
        _front->init (); // avoid double free
        _has_msg = false;
        return ypipe_read_consumed;
    }

    void discard_accounting (uint64_t (*accounted_bytes_) (const msg_t &),
                             bool (*counted_message_) (const msg_t &),
                             uint64_t *discarded_bytes_,
                             uint64_t *discarded_messages_)
    {
        zlink_assert (discarded_bytes_);
        zlink_assert (discarded_messages_);
        *discarded_bytes_ = 0;
        *discarded_messages_ = 0;
        scoped_lock_t lock (_sync);
        const msg_t *held[2] = {_has_msg ? _front : NULL,
                                _has_back_msg ? _back : NULL};
        for (size_t i = 0; i != 2; ++i) {
            if (!held[i])
                continue;
            if (accounted_bytes_) {
                const uint64_t bytes = accounted_bytes_ (*held[i]);
                *discarded_bytes_ = UINT64_MAX - *discarded_bytes_ < bytes
                                      ? UINT64_MAX
                                      : *discarded_bytes_ + bytes;
            }
            if (counted_message_ && counted_message_ (*held[i]))
                ++*discarded_messages_;
        }
    }

  private:
    msg_t _storage[2];
    msg_t *_back, *_front;

    mutable mutex_t _sync;
    bool _has_msg;
    bool _has_back_msg;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (dbuffer_t)
};
}

#endif
