/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstddef>
#include <mutex>
#include <ostream>
#include <streambuf>
#include <string>

namespace control_contract
{

// Thread-safe capture buffer: the connector io thread may write to a redirected
// standard stream while the test runs, and std::stringbuf is not safe for that.
class locked_capture_buf_t final : public std::streambuf
{
  public:
    std::string text () const
    {
        std::lock_guard<std::mutex> lock (_mutex);
        return _text;
    }

  protected:
    int_type overflow (int_type ch) override
    {
        if (!traits_type::eq_int_type (ch, traits_type::eof ())) {
            std::lock_guard<std::mutex> lock (_mutex);
            _text.push_back (traits_type::to_char_type (ch));
        }
        return traits_type::not_eof (ch);
    }

    std::streamsize xsputn (const char *data, std::streamsize size) override
    {
        std::lock_guard<std::mutex> lock (_mutex);
        _text.append (data, static_cast<std::size_t> (size));
        return size;
    }

    int sync () override { return 0; }

  private:
    mutable std::mutex _mutex;
    std::string _text;
};

// Redirects a stream into a locked_capture_buf_t until close () or scope end.
class stream_capture_t final
{
  public:
    explicit stream_capture_t (std::ostream &stream) :
        _stream (stream), _previous (stream.rdbuf (&_buffer))
    {
    }
    stream_capture_t (const stream_capture_t &) = delete;
    stream_capture_t &operator= (const stream_capture_t &) = delete;
    ~stream_capture_t () { close (); }

    void close ()
    {
        if (_previous) {
            _stream.rdbuf (_previous);
            _previous = nullptr;
        }
    }

    std::string text () const { return _buffer.text (); }

  private:
    std::ostream &_stream;
    locked_capture_buf_t _buffer;
    std::streambuf *_previous;
};

}
