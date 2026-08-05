/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ASIO_HANDLER_ALLOCATOR_HPP_INCLUDED__
#define __ZLINK_ASIO_HANDLER_ALLOCATOR_HPP_INCLUDED__

//  Zero-allocation handlers.
//
//  This header provides a custom allocator for ASIO async handlers that
//  avoids heap allocations for small lambda captures. Each poll_entry_t
//  has its own handler_allocator instances, allowing the allocator to
//  recycle memory for the common case of async_wait handlers.
//
//  Expected improvement: 10-15% for high-frequency event patterns
//  like ROUTER multipart messages where callbacks fire rapidly.
//
//  Based on the ASIO "Custom Memory Allocation" example:
//  https://www.boost.org/doc/libs/release/doc/html/boost_asio/example/cpp11/allocation/server.cpp

#include <cstddef>
#include <memory>
#include <type_traits>

namespace zlink
{

//  Recycling allocator for async handlers.
//  Provides a 1024-byte inline buffer to avoid heap allocations for
//  typical lambda captures (pointer + a few state flags).
//
//  Thread-safety: Each handler_allocator should be used by a single
//  poll_entry_t's async operations. No synchronization is needed
//  since ASIO poll/run is single-threaded within an io_context.
class handler_allocator
{
  public:
    handler_allocator () : _in_use (false) {}

    //  Non-copyable and non-movable (owns inline storage)
    handler_allocator (const handler_allocator &) = delete;
    handler_allocator &operator= (const handler_allocator &) = delete;

    void *allocate (std::size_t size)
    {
        if (!_in_use && size <= sizeof (_storage)) {
            _in_use = true;
            return &_storage;
        }
        //  Fall back to heap for larger allocations
        return ::operator new (size);
    }

    void deallocate (void *pointer)
    {
        if (pointer == &_storage) {
            _in_use = false;
        } else {
            ::operator delete (pointer);
        }
    }

  private:
    //  Inline storage for small allocations. 1KB keeps handler-wrapper
    //  allocations off the heap for the common read/write completion path.
    typename std::aligned_storage<1024>::type _storage;
    bool _in_use;
};


template <typename T> class handler_alloc_std_allocator;

template <> class handler_alloc_std_allocator<void>
{
  public:
    typedef void value_type;
    typedef void *pointer;
    typedef const void *const_pointer;

    template <typename U> struct rebind
    {
        typedef handler_alloc_std_allocator<U> other;
    };

    explicit handler_alloc_std_allocator (handler_allocator &alloc) noexcept : _allocator (&alloc)
    {
    }

    template <typename U>
    handler_alloc_std_allocator (const handler_alloc_std_allocator<U> &other) noexcept :
        _allocator (other._allocator)
    {
    }

    bool operator== (const handler_alloc_std_allocator<void> &other) const noexcept
    {
        return _allocator == other._allocator;
    }

    bool operator!= (const handler_alloc_std_allocator<void> &other) const noexcept
    {
        return !(*this == other);
    }

  private:
    template <typename U> friend class handler_alloc_std_allocator;

    handler_allocator *_allocator;
};

template <typename T> class handler_alloc_std_allocator
{
  public:
    typedef T value_type;
    typedef T *pointer;
    typedef const T *const_pointer;
    typedef std::size_t size_type;
    typedef std::ptrdiff_t difference_type;

    template <typename U> struct rebind
    {
        typedef handler_alloc_std_allocator<U> other;
    };

    explicit handler_alloc_std_allocator (handler_allocator &alloc) noexcept : _allocator (&alloc)
    {
    }

    template <typename U>
    handler_alloc_std_allocator (const handler_alloc_std_allocator<U> &other) noexcept :
        _allocator (other._allocator)
    {
    }

    pointer allocate (size_type n)
    {
        return static_cast<pointer> (_allocator->allocate (n * sizeof (T)));
    }

    void deallocate (pointer p, size_type) { _allocator->deallocate (p); }

    bool operator== (const handler_alloc_std_allocator<T> &other) const noexcept
    {
        return _allocator == other._allocator;
    }

    bool operator!= (const handler_alloc_std_allocator<T> &other) const noexcept
    {
        return !(*this == other);
    }

  private:
    template <typename U> friend class handler_alloc_std_allocator;

    handler_allocator *_allocator;
};


//  Custom handler wrapper that uses handler hooks for custom allocation.
//  This wrapper stores a reference to the handler_allocator and allows
//  ASIO to use it via the asio_handler_allocate/deallocate hooks (ADL).
template <typename Handler> class custom_alloc_handler
{
  public:
    custom_alloc_handler (handler_allocator &alloc, Handler h) :
        _allocator (alloc), _handler (std::move (h))
    {
    }

    //  Allow move construction for efficiency
    custom_alloc_handler (custom_alloc_handler &&other) :
        _allocator (other._allocator), _handler (std::move (other._handler))
    {
    }

    //  Copy constructor needed for some ASIO internals
    custom_alloc_handler (const custom_alloc_handler &other) :
        _allocator (other._allocator), _handler (other._handler)
    {
    }

    template <typename... Args> void operator() (Args &&...args)
    {
        _handler (std::forward<Args> (args)...);
    }

    //  Friend functions for ASIO handler hooks (found via ADL)
    //  Note: In newer Boost.Asio versions (1.74+), these hooks may be ignored
    //  in favor of the associated_allocator mechanism, but they still work
    //  as a fallback and provide compatibility with older versions.
    friend void *asio_handler_allocate (std::size_t size, custom_alloc_handler *this_handler)
    {
        return this_handler->_allocator.allocate (size);
    }

    friend void asio_handler_deallocate (void *pointer,
                                         std::size_t /*size*/,
                                         custom_alloc_handler *this_handler)
    {
        this_handler->_allocator.deallocate (pointer);
    }

    //  Associated allocator support (for newer Boost.Asio versions).
    using allocator_type = handler_alloc_std_allocator<void>;
    allocator_type get_allocator () const noexcept { return allocator_type (_allocator); }

  private:
    handler_allocator &_allocator;
    Handler _handler;
};


//  Helper function to create a custom_alloc_handler.
//  Usage:
//    entry_->descriptor.async_wait(
//        wait_read,
//        make_custom_alloc_handler(entry_->in_allocator, [this, entry_](...) { ... }));
template <typename Handler>
inline custom_alloc_handler<Handler> make_custom_alloc_handler (handler_allocator &alloc, Handler h)
{
    return custom_alloc_handler<Handler> (alloc, std::move (h));
}

} // namespace zlink

#endif // __ZLINK_ASIO_HANDLER_ALLOCATOR_HPP_INCLUDED__
