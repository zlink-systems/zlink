/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SOCKET_POLLER_HPP_INCLUDED__
#define __ZLINK_SOCKET_POLLER_HPP_INCLUDED__

#include "core/poller.hpp"

#if defined ZLINK_POLL_BASED_ON_POLL && !defined ZLINK_HAVE_WINDOWS
#include <poll.h>
#endif

#if defined ZLINK_HAVE_WINDOWS
#include "utils/windows.hpp"
#elif defined ZLINK_HAVE_VXWORKS
#include <unistd.h>
#include <sys/time.h>
#include <strings.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cstddef>
#include <new>

#include "sockets/common/socket_base.hpp"
#include "core/signaler.hpp"
#include "utils/polling_util.hpp"

namespace zlink
{
class socket_poller_t
{
  public:
    enum output_readiness_t
    {
        public_output_readiness,
        transport_output_readiness
    };

    explicit socket_poller_t (
      output_readiness_t output_readiness_ = public_output_readiness);
    ~socket_poller_t ();

    struct event_t
    {
        void *socket;
        fd_t fd;
        void *user_data;
        short events;
    };

    //  Stage 1 (plan 7.1): zlink_poll() builds a poller per call and adds
    //  every item, so the item vector reallocated repeatedly while growing.
    //  Callers that know the final size say so up front.
    void reserve (size_t items_);
    int add (socket_base_t *socket_, void *user_data_, short events_);
    int modify (const socket_base_t *socket_, short events_);
    int modify_user_data (const socket_base_t *socket_, void *user_data_);
    int remove (socket_base_t *socket_);

    int add_fd (fd_t fd_, void *user_data_, short events_);
    int modify_fd (fd_t fd_, short events_);
    int modify_fd_user_data (fd_t fd_, void *user_data_);
    int remove_fd (fd_t fd_);
    // Returns an error (signalers are not used).
    int signaler_fd (fd_t *fd_) const;

    int wait (event_t *events_, int n_events_, long timeout_);

    int size () const { return static_cast<int> (_items.size ()); };

    //  Return false if object is not a socket.
    bool check_tag () const;

  private:
    typedef struct item_t
    {
        socket_base_t *socket;
        fd_t fd;
        void *user_data;
        short events;
        bool terminal_event_delivered;
#if defined ZLINK_POLL_BASED_ON_POLL
        int pollfd_index;
#endif
    } item_t;

    // Poll registrations are normally few and live no longer than this
    // poller. Keep their storage with that lifecycle owner; larger pollsets
    // transparently fall back to the heap without changing the poll contract.
    class items_t
    {
      public:
        typedef item_t *iterator;

        items_t () : _data (_inline_items), _size (0), _capacity (inline_capacity) {}

        ~items_t ()
        {
            if (_data != _inline_items)
                delete[] _data;
        }

        void reserve (size_t capacity_)
        {
            if (capacity_ <= _capacity)
                return;

            item_t *const items = new item_t[capacity_];
            std::copy (_data, _data + _size, items);
            if (_data != _inline_items)
                delete[] _data;
            _data = items;
            _capacity = capacity_;
        }

        void push_back (const item_t &item_)
        {
            if (_size == _capacity)
                reserve (_capacity * 2);
            _data[_size++] = item_;
        }

        iterator erase (iterator item_)
        {
            std::copy (item_ + 1, end (), item_);
            --_size;
            return item_;
        }

        iterator begin () { return _data; }
        iterator end () { return _data + _size; }
        size_t size () const { return _size; }
        size_t capacity () const { return _capacity; }
        bool empty () const { return _size == 0; }

      private:
        enum
        {
            inline_capacity = ZLINK_POLLITEMS_DFLT
        };

        item_t _inline_items[inline_capacity];
        item_t *_data;
        size_t _size;
        size_t _capacity;

        ZLINK_NON_COPYABLE_NOR_MOVABLE (items_t)
    };

    static void
    zero_trail_events (zlink::socket_poller_t::event_t *events_, int n_events_, int found_);
    int check_socket_events (zlink::socket_poller_t::event_t *events_, int n_events_);
#if defined ZLINK_POLL_BASED_ON_POLL
    int check_events (zlink::socket_poller_t::event_t *events_, int n_events_);
#elif defined ZLINK_POLL_BASED_ON_SELECT
    int check_events (zlink::socket_poller_t::event_t *events_,
                      int n_events_,
                      fd_set &inset_,
                      fd_set &outset_,
                      fd_set &errset_);
#endif
    static bool is_socket (const item_t &item, const socket_base_t *socket_)
    {
        return item.socket == socket_;
    }
    static bool is_fd (const item_t &item, fd_t fd_) { return !item.socket && item.fd == fd_; }

    int rebuild ();

    //  Used to check whether the object is a socket_poller.
    uint32_t _tag;

    // Public POLLOUT is a backpressure-recovery edge. The in-process proxy
    // selects physical transport writability because it must not consume a
    // source record until its destination can accept it.
    output_readiness_t _output_readiness;

#if defined ZLINK_HAVE_WINDOWS
    // Windows cannot poll the signaler sockets as cheaply as Linux can poll
    // eventfd descriptors. Socket-only pollers use one event shared by all
    // registered mailboxes; pollers containing raw descriptors keep the
    // WSAPoll path below.
    signaler_t _windows_signaler;
    bool _windows_signaler_active;
#else
    // Socket readiness has its own wakeup channel. The mailbox's primary
    // signaler remains owned by the command executor when async dispatch is
    // active, so a poller must never compete for that descriptor.
    //  Created on first use only. The descriptor pollset path never needs it,
    //  so an ordinary poll over sockets allocates no eventfd at all.
    signaler_t *_socket_signaler;
    bool _socket_signaler_active;
#if defined ZLINK_POLL_BASED_ON_POLL
    int _socket_signaler_pollfd_index;
#endif
#endif

    //  List of sockets
    items_t _items;


    items_t::iterator find_socket_item (const socket_base_t *socket_);
    items_t::iterator find_fd_item (fd_t fd_);
    int add_item (socket_base_t *socket_, fd_t fd_, void *user_data_, short events_);
    int modify_item_events (items_t::iterator it_, short events_);
    int modify_item_user_data (items_t::iterator it_, void *user_data_);
    int remove_item (items_t::iterator it_);
    int collect_socket_event (item_t &item_, event_t *event_);
#if !defined ZLINK_HAVE_WINDOWS
    signaler_t *ensure_socket_signaler ();
    void unregister_socket_signaler ();
    void drain_socket_signaler ();
#endif

    //  Does the pollset needs rebuilding?
    bool _need_rebuild;

    //  Size of the pollset
    int _pollset_size;

#if defined ZLINK_POLL_BASED_ON_POLL
    int ensure_pollfds_capacity (size_t capacity_);
    pollfd _inline_pollfds[ZLINK_POLLITEMS_DFLT];
    pollfd *_pollfds;
    size_t _pollfds_capacity;
#elif defined ZLINK_POLL_BASED_ON_SELECT
    resizable_optimized_fd_set_t _pollset_in;
    resizable_optimized_fd_set_t _pollset_out;
    resizable_optimized_fd_set_t _pollset_err;
    zlink::fd_t _max_fd;
#endif

    ZLINK_NON_COPYABLE_NOR_MOVABLE (socket_poller_t)
};
}

#endif
