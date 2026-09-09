/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_DBUFFER_HPP_INCLUDED__
#define __ZLINK_DBUFFER_HPP_INCLUDED__

#include <list>
#include <vector>

#include "utils/mutex.hpp"
#include "core/msg.hpp"
#include "core/ypipe_base.hpp"

namespace zlink
{
//  Single-producer/single-consumer conflation buffer. The producer assembles
//  a complete record before publication. Only unread records of the same
//  topic can be replaced; a reader owns the remainder of a started record.
template <typename T> class dbuffer_t;

template <> class dbuffer_t<msg_t>
{
  public:
    dbuffer_t () {}

    void write (const msg_t &value_, bool incomplete_)
    {
        uint64_t ignored_bytes = 0;
        uint64_t ignored_messages = 0;
        write_with_replacement_accounting (
          value_, incomplete_, NULL, NULL, &ignored_bytes, &ignored_messages);
    }

    void write_with_replacement_accounting (
      const msg_t &value_, bool incomplete_,
      uint64_t (*accounted_bytes_) (const msg_t &),
      bool (*counted_message_) (const msg_t &),
      uint64_t *replaced_bytes_, uint64_t *replaced_messages_)
    {
        zlink_assert (value_.check ());
        zlink_assert (replaced_bytes_ && replaced_messages_);
        *replaced_bytes_ = 0;
        *replaced_messages_ = 0;
        _pending.frames.push_back (value_);
        if (incomplete_)
            return;

        scoped_lock_t lock (_sync);
        for (records_t::iterator it = _records.begin (); it != _records.end ();
             ++it) {
            if (it->read_pos == 0 && same_topic (*it, _pending)) {
                it->account (accounted_bytes_, counted_message_,
                             replaced_bytes_, replaced_messages_);
                _records.erase (it);
                break;
            }
        }
        _records.emplace_back ();
        _records.back ().frames.swap (_pending.frames);
    }

    bool unwrite (msg_t *value_)
    {
        if (_pending.frames.empty ())
            return false;
        *value_ = _pending.frames.back ();
        _pending.frames.pop_back ();
        return true;
    }

    bool read (msg_t *value_, bool *batch_exhausted_ = NULL)
    {
        return read_if (value_, NULL, NULL, batch_exhausted_)
               == ypipe_read_consumed;
    }

    bool check_read ()
    {
        scoped_lock_t lock (_sync);
        return !_records.empty ();
    }

    bool probe_if_published (void (*fn_) (const msg_t &, void *),
                             void *userdata_)
    {
        scoped_lock_t lock (_sync);
        if (_records.empty ())
            return false;
        const record_t &record = _records.front ();
        (*fn_) (record.frames[record.read_pos], userdata_);
        return true;
    }

    ypipe_read_result_t
    read_if (msg_t *value_, bool (*fn_) (const msg_t &, void *), void *userdata_,
             bool *batch_exhausted_ = NULL)
    {
        if (batch_exhausted_)
            *batch_exhausted_ = false;
        if (!value_)
            return ypipe_read_rejected;
        scoped_lock_t lock (_sync);
        if (_records.empty ())
            return ypipe_read_empty;
        record_t &record = _records.front ();
        msg_t &front = record.frames[record.read_pos];
        if (fn_ && !(*fn_) (front, userdata_))
            return ypipe_read_rejected;
        *value_ = front;
        front.init (); // Ownership moves to the reader.
        if (++record.read_pos == record.frames.size ()) {
            _records.pop_front ();
            if (batch_exhausted_)
                *batch_exhausted_ = true;
        }
        return ypipe_read_consumed;
    }

    void discard_accounting (uint64_t (*accounted_bytes_) (const msg_t &),
                             bool (*counted_message_) (const msg_t &),
                             uint64_t *discarded_bytes_,
                             uint64_t *discarded_messages_)
    {
        zlink_assert (discarded_bytes_ && discarded_messages_);
        *discarded_bytes_ = 0;
        *discarded_messages_ = 0;
        scoped_lock_t lock (_sync);
        // Pending frames have provisional accounting, released by rollback.
        for (records_t::const_iterator it = _records.begin ();
             it != _records.end (); ++it)
            it->account (accounted_bytes_, counted_message_, discarded_bytes_,
                         discarded_messages_);
    }

  private:
    struct record_t
    {
        record_t () : read_pos (0) {}
        ~record_t ()
        {
            for (size_t i = read_pos; i != frames.size (); ++i) {
                const int rc = frames[i].close ();
                zlink_assert (rc == 0);
            }
        }

        void account (uint64_t (*bytes_) (const msg_t &),
                      bool (*messages_) (const msg_t &), uint64_t *total_bytes_,
                      uint64_t *total_messages_) const
        {
            for (size_t i = read_pos; i != frames.size (); ++i) {
                if (bytes_) {
                    const uint64_t bytes = bytes_ (frames[i]);
                    *total_bytes_ = UINT64_MAX - *total_bytes_ < bytes
                                      ? UINT64_MAX : *total_bytes_ + bytes;
                }
                if (messages_ && messages_ (frames[i]))
                    ++*total_messages_;
            }
        }

        std::vector<msg_t> frames;
        size_t read_pos;
        ZLINK_NON_COPYABLE_NOR_MOVABLE (record_t)
    };

    static bool same_topic (const record_t &left_, const record_t &right_)
    {
        const msg_t &left = left_.frames.front ();
        const msg_t &right = right_.frames.front ();
        // Transport/control records keep their FIFO position and ownership.
        const unsigned char control = msg_t::command | msg_t::routing_id
                                      | msg_t::credential;
        if (left.is_delimiter () || right.is_delimiter ()
            || (left.flags () & control) || (right.flags () & control)
            || left.is_subscribe () || left.is_cancel ()
            || right.is_subscribe () || right.is_cancel ())
            return false;
        const bool left_topic = (left.flags () & msg_t::more) != 0;
        const bool right_topic = (right.flags () & msg_t::more) != 0;
        if (left_topic != right_topic)
            return false;
        return !left_topic
               || (left.size () == right.size ()
                   && (left.size () == 0
                       || memcmp (const_cast<msg_t &> (left).data (),
                                  const_cast<msg_t &> (right).data (),
                                  left.size ())
                            == 0));
    }

    typedef std::list<record_t> records_t;
    record_t _pending;
    records_t _records;
    mutex_t _sync;
    ZLINK_NON_COPYABLE_NOR_MOVABLE (dbuffer_t)
};
}

#endif
