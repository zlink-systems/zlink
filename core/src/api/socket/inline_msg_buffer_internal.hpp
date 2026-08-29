/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_INLINE_MSG_BUFFER_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_INLINE_MSG_BUFFER_INTERNAL_HPP_INCLUDED__

#include <algorithm>
#include <cstddef>
#include <new>
#include <type_traits>

#include <zlink.h>

namespace zlink
{
namespace socket_internal
{
// zlink_msg_t is an opaque, trivially relocatable C handle.  Its lifetime is
// managed explicitly with zlink_msg_init()/zlink_msg_close(), so this buffer
// owns storage only: callers remain responsible for closing live elements.
// Keeping the common case inline avoids a per-record allocation without
// requiring Boost.Container in the public Core build environment.
template <size_t InlineCapacity> class inline_msg_buffer_t
{
  public:
    inline_msg_buffer_t () : _data (_inline), _size (0), _capacity (InlineCapacity)
    {
        static_assert (InlineCapacity > 0, "inline message capacity must be positive");
        static_assert (std::is_trivially_copyable<zlink_msg_t>::value,
                       "inline message storage requires relocatable handles");
    }

    ~inline_msg_buffer_t ()
    {
        if (_data != _inline)
            delete[] _data;
    }

    void reserve (size_t capacity_)
    {
        if (capacity_ <= _capacity)
            return;

        zlink_msg_t *const replacement = new zlink_msg_t[capacity_];
        std::copy (_data, _data + _size, replacement);
        if (_data != _inline)
            delete[] _data;
        _data = replacement;
        _capacity = capacity_;
    }

    void resize (size_t size_)
    {
        if (size_ > _capacity) {
            size_t grown_capacity = _capacity * 2;
            if (grown_capacity < _capacity || grown_capacity < size_)
                grown_capacity = size_;
            reserve (grown_capacity);
        }
        _size = size_;
    }

    zlink_msg_t &append_uninitialized ()
    {
        if (_size == static_cast<size_t> (-1))
            throw std::bad_array_new_length ();
        const size_t old_size = _size;
        resize (old_size + 1);
        return _data[old_size];
    }

    void pop_back ()
    {
        if (_size != 0)
            --_size;
    }

    void clear () { _size = 0; }

    bool empty () const { return _size == 0; }
    size_t size () const { return _size; }
    size_t capacity () const { return _capacity; }

    zlink_msg_t *data () { return _data; }
    const zlink_msg_t *data () const { return _data; }

    zlink_msg_t &back () { return _data[_size - 1]; }
    const zlink_msg_t &back () const { return _data[_size - 1]; }

    zlink_msg_t &operator[] (size_t index_) { return _data[index_]; }
    const zlink_msg_t &operator[] (size_t index_) const { return _data[index_]; }

  private:
    zlink_msg_t _inline[InlineCapacity];
    zlink_msg_t *_data;
    size_t _size;
    size_t _capacity;

    inline_msg_buffer_t (const inline_msg_buffer_t &) = delete;
    inline_msg_buffer_t &operator= (const inline_msg_buffer_t &) = delete;
};
}
}

#endif
