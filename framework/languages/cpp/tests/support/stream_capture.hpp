/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <iostream>
#include <mutex>
#include <ostream>
#include <streambuf>
#include <string>

namespace zlink::framework::test
{

// Thread-safe capture buffer. std::basic_stringbuf is not safe for concurrent
// writers, and Framework worker threads write to std::cerr while a host runs
// and shuts down, so a test that redirects a standard stream across a run uses
// this buffer instead of an ostringstream.
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

// Redirects a standard stream into a locked_capture_buf_t until close () or the
// end of the scope, then restores the previous buffer. Call close () only after
// every thread that writes to the stream has finished its writes for the run
// under test (for example after app.run () returned); text () stays readable.
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
