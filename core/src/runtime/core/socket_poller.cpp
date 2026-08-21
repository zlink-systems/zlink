/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "core/socket_poller.hpp"
#include "core/mailbox.hpp"
#include "utils/err.hpp"
#include "utils/polling_util.hpp"
#include "utils/macros.hpp"

#include <limits.h>

// compare elements to value
template <class It, class T, class Pred>
static It find_if2 (It b_, It e_, const T &value, Pred pred)
{
    for (; b_ != e_; ++b_) {
        if (pred (*b_, value)) {
            break;
        }
    }
    return b_;
}

zlink::socket_poller_t::socket_poller_t () :
    _tag (0xCAFEBABE)
#if defined ZLINK_HAVE_WINDOWS
    ,
    _windows_signaler (true),
    _windows_signaler_active (false)
#else
    ,
    _socket_signaler (NULL),
    _socket_signaler_active (false)
#if defined ZLINK_POLL_BASED_ON_POLL
    ,
    _socket_signaler_pollfd_index (-1)
#endif
#endif
#if defined ZLINK_POLL_BASED_ON_POLL
    ,
    _pollfds (NULL)
#elif defined ZLINK_POLL_BASED_ON_SELECT
    ,
    _max_fd (0)
#endif
{
    rebuild ();
}

zlink::socket_poller_t::~socket_poller_t ()
{
#if defined ZLINK_HAVE_WINDOWS
    if (_windows_signaler_active) {
        for (items_t::iterator it = _items.begin (), end = _items.end (); it != end; ++it) {
            if (it->socket)
                static_cast<mailbox_t *> (it->socket->get_mailbox ())->remove_signaler (
                  &_windows_signaler);
        }
        _windows_signaler_active = false;
    }
    _windows_signaler.reset_event ();
#else
    unregister_socket_signaler ();
    drain_socket_signaler ();
    delete _socket_signaler;
    _socket_signaler = NULL;
#endif

    //  Mark the socket_poller as dead
    _tag = 0xdeadbeef;

#if defined ZLINK_POLL_BASED_ON_POLL
    if (_pollfds) {
        free (_pollfds);
        _pollfds = NULL;
    }
#endif
}

bool zlink::socket_poller_t::check_tag () const
{
    return _tag == 0xCAFEBABE;
}

int zlink::socket_poller_t::signaler_fd (fd_t *fd_) const
{
    (void) fd_;
    errno = EINVAL;
    return -1;
}

int zlink::socket_poller_t::add (socket_base_t *socket_, void *user_data_, short events_)
{
    if (_socket_index.find (socket_) != _socket_index.end ()) {
        errno = EINVAL;
        return -1;
    }
    return add_item (socket_, 0, user_data_, events_);
}

int zlink::socket_poller_t::add_fd (fd_t fd_, void *user_data_, short events_)
{
    if (find_fd_item (fd_) != _items.end ()) {
        errno = EINVAL;
        return -1;
    }
    return add_item (NULL, fd_, user_data_, events_);
}

int zlink::socket_poller_t::add_item (socket_base_t *socket_,
                                      fd_t fd_,
                                      void *user_data_,
                                      short events_)
{
    const item_t item = {socket_, fd_, user_data_, events_, false
#if defined ZLINK_POLL_BASED_ON_POLL
                         ,
                         -1
#endif
    };
    try {
        _items.push_back (item);
        if (socket_)
            _socket_index.insert (socket_);
    }
    catch (const std::bad_alloc &) {
        errno = ENOMEM;
        return -1;
    }
    _need_rebuild = true;

    return 0;
}

int zlink::socket_poller_t::modify (const socket_base_t *socket_, short events_)
{
    return modify_item_events (find_socket_item (socket_), events_);
}

int zlink::socket_poller_t::modify_user_data (const socket_base_t *socket_, void *user_data_)
{
    return modify_item_user_data (find_socket_item (socket_), user_data_);
}


int zlink::socket_poller_t::modify_fd (fd_t fd_, short events_)
{
    return modify_item_events (find_fd_item (fd_), events_);
}

int zlink::socket_poller_t::modify_fd_user_data (fd_t fd_, void *user_data_)
{
    return modify_item_user_data (find_fd_item (fd_), user_data_);
}


int zlink::socket_poller_t::remove (socket_base_t *socket_)
{
    return remove_item (find_socket_item (socket_));
}

int zlink::socket_poller_t::remove_fd (fd_t fd_)
{
    return remove_item (find_fd_item (fd_));
}

zlink::socket_poller_t::items_t::iterator
zlink::socket_poller_t::find_socket_item (const socket_base_t *socket_)
{
    return find_if2 (_items.begin (), _items.end (), socket_, &is_socket);
}

zlink::socket_poller_t::items_t::iterator zlink::socket_poller_t::find_fd_item (fd_t fd_)
{
    return find_if2 (_items.begin (), _items.end (), fd_, &is_fd);
}

int zlink::socket_poller_t::modify_item_events (items_t::iterator it_, short events_)
{
    if (it_ == _items.end ()) {
        errno = EINVAL;
        return -1;
    }

    if (it_->events == events_)
        return 0;

    it_->events = events_;
    _need_rebuild = true;

    return 0;
}

int zlink::socket_poller_t::modify_item_user_data (items_t::iterator it_, void *user_data_)
{
    if (it_ == _items.end ()) {
        errno = EINVAL;
        return -1;
    }

    it_->user_data = user_data_;
    return 0;
}

int zlink::socket_poller_t::remove_item (items_t::iterator it_)
{
    if (it_ == _items.end ()) {
        errno = EINVAL;
        return -1;
    }

#if defined ZLINK_HAVE_WINDOWS
    if (_windows_signaler_active && it_->socket)
        static_cast<mailbox_t *> (it_->socket->get_mailbox ())->remove_signaler (
          &_windows_signaler);
#else
    if (_socket_signaler_active && _socket_signaler && it_->socket)
        static_cast<mailbox_t *> (it_->socket->get_mailbox ())->remove_signaler (
          _socket_signaler);
#endif

    if (it_->socket)
        _socket_index.erase (it_->socket);
    _items.erase (it_);
    _need_rebuild = true;

    return 0;
}

//  Stage 1 (plan 7.1): before 3ef4d09a37 the POSIX poller placed each socket's
//  own mailbox descriptor in the pollset. That commit replaced it with a
//  poller-owned shared signaler that every rebuild registers into, and
//  unregisters from, every socket mailbox. zlink_poll() builds a fresh poller
//  per call, so a 100-socket poll paid 200 mailbox-mutex round trips that
//  contend with the I/O threads delivering commands. Restore the descriptor
//  pollset.
#define ZLINK_STAGE1_POLLER_PARENT_PATH 1

int zlink::socket_poller_t::rebuild ()
{
#if defined ZLINK_HAVE_WINDOWS
    if (_windows_signaler_active) {
        for (items_t::iterator it = _items.begin (), end = _items.end (); it != end; ++it) {
            if (it->socket)
                static_cast<mailbox_t *> (it->socket->get_mailbox ())->remove_signaler (
                  &_windows_signaler);
        }
        _windows_signaler_active = false;
        _windows_signaler.reset_event ();
    }
#else
    unregister_socket_signaler ();
    drain_socket_signaler ();
#endif

    _pollset_size = 0;
    _need_rebuild = false;

#if defined ZLINK_HAVE_WINDOWS
    bool windows_socket_only = true;
    for (items_t::iterator it = _items.begin (), end = _items.end (); it != end; ++it) {
        if (!it->events)
            continue;
        ++_pollset_size;
        if (!it->socket)
            windows_socket_only = false;
    }

    if (_pollset_size > 0 && windows_socket_only) {
        for (items_t::iterator it = _items.begin (), end = _items.end (); it != end; ++it) {
            if (it->events)
                static_cast<mailbox_t *> (it->socket->get_mailbox ())->add_signaler (
                  &_windows_signaler);
        }
        _windows_signaler_active = true;
        return 0;
    }

    // The platform-specific fallback below rebuilds the complete pollset.
    _pollset_size = 0;
#else
#if ZLINK_STAGE1_POLLER_PARENT_PATH
    for (items_t::iterator it = _items.begin (), end = _items.end (); it != end; ++it) {
        if (it->events)
            ++_pollset_size;
    }
#else
    bool has_socket_items = false;
    for (items_t::iterator it = _items.begin (), end = _items.end (); it != end; ++it) {
        if (!it->events)
            continue;
        if (it->socket)
            has_socket_items = true;
        else
            ++_pollset_size;
    }
    if (has_socket_items) {
        if (!ensure_socket_signaler () || !_socket_signaler->valid ()) {
            errno = EMFILE;
            _need_rebuild = true;
            return -1;
        }
        for (items_t::iterator it = _items.begin (), end = _items.end (); it != end; ++it) {
            if (it->socket && it->events) {
                mailbox_t *const mailbox = static_cast<mailbox_t *> (it->socket->get_mailbox ());
                // Keep the primary signaler assigned to the mailbox command
                // owner. Only the poller-owned secondary descriptor enters
                // the platform pollset below.
                (void) mailbox->get_fd ();
                mailbox->add_signaler (_socket_signaler);
            }
        }
        _socket_signaler_active = true;
        ++_pollset_size;
    }
#endif
#endif

#if defined ZLINK_POLL_BASED_ON_POLL

    if (_pollfds) {
        free (_pollfds);
        _pollfds = NULL;
    }

#if defined ZLINK_HAVE_WINDOWS
    for (items_t::iterator it = _items.begin (), end = _items.end (); it != end; ++it) {
        if (it->events)
            _pollset_size++;
    }
#endif

    if (_pollset_size == 0)
        return 0;

    _pollfds = static_cast<pollfd *> (malloc (_pollset_size * sizeof (pollfd)));

    if (!_pollfds) {
        errno = ENOMEM;
        _need_rebuild = true;
        return -1;
    }

    int item_nbr = 0;

#if !defined ZLINK_HAVE_WINDOWS
    _socket_signaler_pollfd_index = -1;
    if (!ZLINK_STAGE1_POLLER_PARENT_PATH && _socket_signaler_active) {
        _socket_signaler_pollfd_index = item_nbr;
        _pollfds[item_nbr].fd = _socket_signaler->get_fd ();
        _pollfds[item_nbr].events = POLLIN;
        ++item_nbr;
    }
#endif

    for (items_t::iterator it = _items.begin (), end = _items.end (); it != end; ++it) {
        if (it->events) {
            if (it->socket) {
#if !defined ZLINK_HAVE_WINDOWS && !ZLINK_STAGE1_POLLER_PARENT_PATH
                continue;
#else
                size_t fd_size = sizeof (zlink::fd_t);
                const int rc =
                  it->socket->getsockopt (ZLINK_INTERNAL_OPT_FD, &_pollfds[item_nbr].fd, &fd_size);
                if (rc != 0) {
                    const int saved_errno = errno;
                    free (_pollfds);
                    _pollfds = NULL;
                    _pollset_size = 0;
                    _need_rebuild = true;
                    errno = saved_errno ? saved_errno : ETERM;
                    return -1;
                }

                _pollfds[item_nbr].events = POLLIN;
                item_nbr++;
#endif
            } else {
                _pollfds[item_nbr].fd = it->fd;
                _pollfds[item_nbr].events = (it->events & ZLINK_POLLIN ? POLLIN : 0)
                                            | (it->events & ZLINK_POLLOUT ? POLLOUT : 0)
                                            | (it->events & ZLINK_POLLPRI ? POLLPRI : 0);
                it->pollfd_index = item_nbr;
                item_nbr++;
            }
        }
    }

#elif defined ZLINK_POLL_BASED_ON_SELECT

    //  Ensure we do not attempt to select () on more than FD_SETSIZE
    //  file descriptors.
    zlink_assert (_items.size () <= FD_SETSIZE);

    _pollset_in.resize (_items.size ());
    _pollset_out.resize (_items.size ());
    _pollset_err.resize (_items.size ());

    FD_ZERO (_pollset_in.get ());
    FD_ZERO (_pollset_out.get ());
    FD_ZERO (_pollset_err.get ());

    _max_fd = 0;

#if !defined ZLINK_HAVE_WINDOWS
    if (!ZLINK_STAGE1_POLLER_PARENT_PATH && _socket_signaler_active) {
        const zlink::fd_t socket_signaler_fd = _socket_signaler->get_fd ();
        FD_SET (socket_signaler_fd, _pollset_in.get ());
        if (_max_fd < socket_signaler_fd)
            _max_fd = socket_signaler_fd;
    }
#endif

    //  Build the fd_sets for passing to select ().
    for (items_t::iterator it = _items.begin (), end = _items.end (); it != end; ++it) {
        if (it->events) {
            //  If the poll item is a 0MQ socket we are interested in input on the
            //  notification file descriptor retrieved by the ZLINK_INTERNAL_OPT_FD socket option.
            if (it->socket) {
#if !defined ZLINK_HAVE_WINDOWS && !ZLINK_STAGE1_POLLER_PARENT_PATH
                continue;
#else
                zlink::fd_t notify_fd;
                size_t fd_size = sizeof (zlink::fd_t);
                int rc = it->socket->getsockopt (ZLINK_INTERNAL_OPT_FD, &notify_fd, &fd_size);
                if (rc != 0) {
                    _pollset_size = 0;
                    _need_rebuild = true;
                    errno = errno ? errno : ETERM;
                    return -1;
                }

                FD_SET (notify_fd, _pollset_in.get ());
                if (_max_fd < notify_fd)
                    _max_fd = notify_fd;

                _pollset_size++;
#endif
            }
            //  Else, the poll item is a raw file descriptor. Convert the poll item
            //  events to the appropriate fd_sets.
            else {
                if (it->events & ZLINK_POLLIN)
                    FD_SET (it->fd, _pollset_in.get ());
                if (it->events & ZLINK_POLLOUT)
                    FD_SET (it->fd, _pollset_out.get ());
                if (it->events & ZLINK_POLLERR)
                    FD_SET (it->fd, _pollset_err.get ());
                if (_max_fd < it->fd)
                    _max_fd = it->fd;

#if defined ZLINK_HAVE_WINDOWS
                _pollset_size++;
#endif
            }
        }
    }

#endif

    return 0;
}

#if !defined ZLINK_HAVE_WINDOWS
zlink::signaler_t *zlink::socket_poller_t::ensure_socket_signaler ()
{
    if (!_socket_signaler)
        _socket_signaler = new (std::nothrow) signaler_t (true);
    return _socket_signaler;
}

void zlink::socket_poller_t::unregister_socket_signaler ()
{
    if (!_socket_signaler_active || !_socket_signaler)
        return;
    for (items_t::iterator it = _items.begin (), end = _items.end (); it != end; ++it) {
        if (it->socket)
            static_cast<mailbox_t *> (it->socket->get_mailbox ())
              ->remove_signaler (_socket_signaler);
    }
    _socket_signaler_active = false;
}

void zlink::socket_poller_t::drain_socket_signaler ()
{
    if (!_socket_signaler)
        return;
    while (_socket_signaler->recv_failable () == 0) {
    }
}
#endif

void zlink::socket_poller_t::zero_trail_events (zlink::socket_poller_t::event_t *events_,
                                                int n_events_,
                                                int found_)
{
    for (int i = found_; i < n_events_; ++i) {
        events_[i].socket = NULL;
        events_[i].fd = zlink::retired_fd;
        events_[i].user_data = NULL;
        events_[i].events = 0;
    }
}

int zlink::socket_poller_t::collect_socket_event (item_t &item_, event_t *event_)
{
    if (item_.terminal_event_delivered)
        return 0;

    uint32_t events;
    if (item_.socket->get_events_for_poller (item_.events, &events) == -1) {
        return -1;
    }

    if (events & ZLINK_POLLERR) {
        item_.terminal_event_delivered = true;
        event_->socket = item_.socket;
        event_->fd = zlink::retired_fd;
        event_->user_data = item_.user_data;
        event_->events = ZLINK_POLLERR;
        return 1;
    }

    if (!(item_.events & events))
        return 0;

    event_->socket = item_.socket;
    event_->fd = zlink::retired_fd;
    event_->user_data = item_.user_data;
    event_->events = item_.events & events;
    return 1;
}

int zlink::socket_poller_t::check_socket_events (zlink::socket_poller_t::event_t *events_,
                                                 int n_events_)
{
    int found = 0;
    for (items_t::iterator it = _items.begin (), end = _items.end ();
         it != end && found < n_events_; ++it) {
        if (!it->socket)
            continue;

        const int socket_event = collect_socket_event (*it, &events_[found]);
        if (socket_event == -1)
            return -1;
        if (socket_event > 0)
            ++found;
    }
    return found;
}

#if defined ZLINK_POLL_BASED_ON_POLL
int zlink::socket_poller_t::check_events (zlink::socket_poller_t::event_t *events_, int n_events_)
#elif defined ZLINK_POLL_BASED_ON_SELECT
int zlink::socket_poller_t::check_events (zlink::socket_poller_t::event_t *events_,
                                          int n_events_,
                                          fd_set &inset_,
                                          fd_set &outset_,
                                          fd_set &errset_)
#endif
{
    int found = 0;
    for (items_t::iterator it = _items.begin (), end = _items.end ();
         it != end && found < n_events_; ++it) {
        //  The poll item is a 0MQ socket. Retrieve pending events
        //  using the ZLINK_INTERNAL_OPT_EVENTS socket option.
        if (it->socket) {
            const int socket_event = collect_socket_event (*it, &events_[found]);
            if (socket_event == -1)
                return -1;
            if (socket_event > 0)
                ++found;
        }
        //  Else, the poll item is a raw file descriptor, simply convert
        //  the events to zlink_pollitem_t-style format.
        else if (it->events) {
#if defined ZLINK_POLL_BASED_ON_POLL
            zlink_assert (it->pollfd_index >= 0);
            const short revents = _pollfds[it->pollfd_index].revents;
            short events = 0;

            if (revents & POLLIN)
                events |= ZLINK_POLLIN;
            if (revents & POLLOUT)
                events |= ZLINK_POLLOUT;
            if (revents & POLLPRI)
                events |= ZLINK_POLLPRI;
            if (revents & ~(POLLIN | POLLOUT | POLLPRI))
                events |= ZLINK_POLLERR;

#elif defined ZLINK_POLL_BASED_ON_SELECT

            short events = 0;

            if (FD_ISSET (it->fd, &inset_))
                events |= ZLINK_POLLIN;
            if (FD_ISSET (it->fd, &outset_))
                events |= ZLINK_POLLOUT;
            if (FD_ISSET (it->fd, &errset_))
                events |= ZLINK_POLLERR;
#endif //POLL_SELECT

            if (events) {
                events_[found].socket = NULL;
                events_[found].fd = it->fd;
                events_[found].user_data = it->user_data;
                events_[found].events = events;
                ++found;
            }
        }
    }

    return found;
}

int zlink::socket_poller_t::wait (zlink::socket_poller_t::event_t *events_,
                                  int n_events_,
                                  long timeout_)
{
    if (_items.empty () && timeout_ < 0) {
        errno = EFAULT;
        return -1;
    }

    if (_need_rebuild) {
        const int rc = rebuild ();
        if (rc == -1)
            return -1;
    }

    if (unlikely (_pollset_size == 0)) {
        if (timeout_ < 0) {
            // Fail instead of trying to sleep forever
            errno = EFAULT;
            return -1;
        }
        // We'll report an error (timed out) as if the list was non-empty and
        // no event occurred within the specified timeout. Otherwise the caller
        // needs to check the return value AND the event to avoid using the
        // nullified event data.
        errno = EAGAIN;
        if (timeout_ == 0)
            return -1;
#if defined ZLINK_HAVE_WINDOWS
        Sleep (timeout_ > 0 ? timeout_ : INFINITE);
        return -1;
#elif defined ZLINK_HAVE_ANDROID
        usleep (timeout_ * 1000);
        return -1;
#elif defined ZLINK_HAVE_OSX
        usleep (timeout_ * 1000);
        errno = EAGAIN;
        return -1;
#elif defined ZLINK_HAVE_VXWORKS
        struct timespec ns_;
        ns_.tv_sec = timeout_ / 1000;
        ns_.tv_nsec = timeout_ % 1000 * 1000000;
        nanosleep (&ns_, 0);
        return -1;
#else
        usleep (timeout_ * 1000);
        return -1;
#endif
    }

#if defined ZLINK_HAVE_WINDOWS
    if (_windows_signaler_active) {
        zlink::clock_t clock;
        uint64_t now = 0;
        uint64_t end = 0;

        while (true) {
            const int socket_events = check_socket_events (events_, n_events_);
            if (socket_events) {
                if (socket_events > 0)
                    zero_trail_events (events_, n_events_, socket_events);
                return socket_events;
            }

            int timeout;
            if (timeout_ == 0)
                timeout = 0;
            else if (timeout_ < 0)
                timeout = -1;
            else {
                if (end == 0) {
                    now = clock.now_ms ();
                    end = now + timeout_;
                }
                const uint64_t remaining = end > now ? end - now : 0;
                timeout = static_cast<int> (std::min<uint64_t> (remaining, INT_MAX));
            }

            const int rc = _windows_signaler.wait (timeout);
            if (rc == -1 && errno != EAGAIN)
                return -1;

            const int found = check_socket_events (events_, n_events_);
            if (found) {
                if (found > 0)
                    zero_trail_events (events_, n_events_, found);
                return found;
            }

            if (timeout_ == 0)
                break;
            if (timeout_ > 0) {
                now = clock.now_ms ();
                if (now >= end)
                    break;
            }
        }
        errno = EAGAIN;
        return -1;
    }
#endif

#if defined ZLINK_POLL_BASED_ON_POLL
    zlink::clock_t clock;
    uint64_t now = 0;
    uint64_t end = 0;

    while (true) {
        const int socket_events = check_socket_events (events_, n_events_);
        if (socket_events) {
            if (socket_events > 0)
                zero_trail_events (events_, n_events_, socket_events);
            return socket_events;
        }

        int timeout;
        if (timeout_ == 0)
            timeout = 0;
        else if (timeout_ < 0)
            timeout = -1;
        else {
            if (end == 0) {
                now = clock.now_ms ();
                end = now + timeout_;
            }
            const uint64_t remaining = end > now ? end - now : 0;
            timeout = static_cast<int> (std::min<uint64_t> (remaining, INT_MAX));
        }

        //  Wait for events.
        const int rc = poll (_pollfds, _pollset_size, timeout);
        if (rc == -1 && errno == EINTR) {
            return -1;
        }
        errno_assert (rc >= 0);

#if !defined ZLINK_HAVE_WINDOWS
        if (_socket_signaler_pollfd_index >= 0
            && (_pollfds[_socket_signaler_pollfd_index].revents & POLLIN))
            drain_socket_signaler ();
#endif

        //  Check for the events.
        const int found = check_events (events_, n_events_);
        if (found) {
            if (found > 0)
                zero_trail_events (events_, n_events_, found);
            return found;
        }

        if (timeout_ == 0)
            break;
        if (timeout_ > 0) {
            now = clock.now_ms ();
            if (now >= end)
                break;
        }
    }
    errno = EAGAIN;
    return -1;

#elif defined ZLINK_POLL_BASED_ON_SELECT

    zlink::clock_t clock;
    uint64_t now = 0;
    uint64_t end = 0;

    optimized_fd_set_t inset (_pollset_size);
    optimized_fd_set_t outset (_pollset_size);
    optimized_fd_set_t errset (_pollset_size);

    while (true) {
        const int socket_events = check_socket_events (events_, n_events_);
        if (socket_events) {
            if (socket_events > 0)
                zero_trail_events (events_, n_events_, socket_events);
            return socket_events;
        }

        timeval timeout;
        timeval *ptimeout;
        if (timeout_ == 0) {
            timeout.tv_sec = 0;
            timeout.tv_usec = 0;
            ptimeout = &timeout;
        } else if (timeout_ < 0)
            ptimeout = NULL;
        else {
            if (end == 0) {
                now = clock.now_ms ();
                end = now + timeout_;
            }
            const uint64_t remaining = end > now ? end - now : 0;
            timeout.tv_sec = static_cast<long> (remaining / 1000);
            timeout.tv_usec = static_cast<long> (remaining % 1000 * 1000);
            ptimeout = &timeout;
        }

        //  Wait for events. Ignore interrupts if there's infinite timeout.
        memcpy (inset.get (), _pollset_in.get (), valid_pollset_bytes (*_pollset_in.get ()));
        memcpy (outset.get (), _pollset_out.get (), valid_pollset_bytes (*_pollset_out.get ()));
        memcpy (errset.get (), _pollset_err.get (), valid_pollset_bytes (*_pollset_err.get ()));
        const int rc = select (static_cast<int> (_max_fd + 1), inset.get (), outset.get (),
                               errset.get (), ptimeout);
#if defined ZLINK_HAVE_WINDOWS
        if (unlikely (rc == SOCKET_ERROR)) {
            errno = wsa_error_to_errno (WSAGetLastError ());
            wsa_assert (errno == ENOTSOCK);
            return -1;
        }
#else
        if (unlikely (rc == -1)) {
            errno_assert (errno == EINTR || errno == EBADF);
            return -1;
        }
#endif

#if !defined ZLINK_HAVE_WINDOWS
        if (_socket_signaler_active && _socket_signaler
            && FD_ISSET (_socket_signaler->get_fd (), inset.get ()))
            drain_socket_signaler ();
#endif

        //  Check for the events.
        const int found =
          check_events (events_, n_events_, *inset.get (), *outset.get (), *errset.get ());
        if (found) {
            if (found > 0)
                zero_trail_events (events_, n_events_, found);
            return found;
        }

        if (timeout_ == 0)
            break;
        if (timeout_ > 0) {
            now = clock.now_ms ();
            if (now >= end)
                break;
        }
    }

    errno = EAGAIN;
    return -1;

#else

    //  Exotic platforms that support neither poll() nor select().
    errno = ENOTSUP;
    return -1;

#endif
}
