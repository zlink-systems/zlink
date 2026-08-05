/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector.hpp>
#include <zlink/stream_e2e_client/task.hpp>

#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <utility>
#include <vector>

namespace zlink::stream_e2e_client
{

using zlink::stream_connector::codec_t;
using zlink::stream_connector::connector_t;
using zlink::stream_connector::expect_none_call_t;
using zlink::stream_connector::metadata_t;
using zlink::stream_connector::packet_t;
using zlink::stream_connector::request_call_t;
using zlink::stream_connector::result_t;
using zlink::stream_connector::send_call_t;
using zlink::stream_connector::wait_call_t;
using zlink::stream_connector::wait_for_sequence_call_t;

class coroutine_send_call_t
{
  public:
    explicit coroutine_send_call_t (send_call_t inner) : _inner (std::move (inner)) {}

    coroutine_send_call_t &packet_name (std::string name)
    {
        _inner.packet_name (std::move (name));
        return *this;
    }

    coroutine_send_call_t &metadata (std::string key, std::string value)
    {
        _inner.metadata (std::move (key), std::move (value));
        return *this;
    }

    coroutine_send_call_t &metadata (metadata_t metadata)
    {
        _inner.metadata (std::move (metadata));
        return *this;
    }

    coroutine_send_call_t &codec (codec_t codec)
    {
        _inner.codec (codec);
        return *this;
    }

    coroutine_send_call_t &compress ()
    {
        _inner.compress ();
        return *this;
    }

    void submit () { _inner.submit (); }

  private:
    send_call_t _inner;
};

class coroutine_request_call_t
{
  public:
    explicit coroutine_request_call_t (request_call_t inner) : _inner (std::move (inner)) {}

    coroutine_request_call_t &packet_name (std::string name)
    {
        _inner.packet_name (std::move (name));
        return *this;
    }

    coroutine_request_call_t &metadata (std::string key, std::string value)
    {
        _inner.metadata (std::move (key), std::move (value));
        return *this;
    }

    coroutine_request_call_t &metadata (metadata_t metadata)
    {
        _inner.metadata (std::move (metadata));
        return *this;
    }

    coroutine_request_call_t &codec (codec_t codec)
    {
        _inner.codec (codec);
        return *this;
    }

    coroutine_request_call_t &timeout (std::chrono::milliseconds timeout)
    {
        _inner.timeout (timeout);
        return *this;
    }

    coroutine_request_call_t &compress ()
    {
        _inner.compress ();
        return *this;
    }

    template <typename TReply> result_t<TReply> submit ()
    {
        return _inner.template submit<TReply> ();
    }

    template <typename TReply> void submit (std::function<void (result_t<TReply>)> callback)
    {
        _inner.template submit<TReply> (std::move (callback));
    }

    template <typename TReply> task_t<TReply> async ()
    {
        auto task = task_t<TReply> (
          [inner = std::move (_inner)] (std::function<void (result_t<TReply>)> callback) mutable {
              inner.template submit<TReply> (std::move (callback));
          });
        task.start ();
        return task;
    }

  private:
    request_call_t _inner;
};

template <typename TMessage> class coroutine_wait_call_t
{
  public:
    explicit coroutine_wait_call_t (wait_call_t<TMessage> inner) : _inner (std::move (inner)) {}

    coroutine_wait_call_t &packet_name (std::string name)
    {
        _inner.packet_name (std::move (name));
        return *this;
    }

    coroutine_wait_call_t &timeout (std::chrono::milliseconds timeout)
    {
        _inner.timeout (timeout);
        return *this;
    }

    coroutine_wait_call_t &where (std::function<bool (const TMessage &)> predicate)
    {
        _inner.where (std::move (predicate));
        return *this;
    }

    template <typename TValue, typename TExpected>
    coroutine_wait_call_t &where (TValue TMessage::*member, TExpected &&expected)
    {
        _inner.where (member, std::forward<TExpected> (expected));
        return *this;
    }

    result_t<TMessage> submit () { return _inner.submit (); }

    void submit (std::function<void (result_t<TMessage>)> callback)
    {
        _inner.submit (std::move (callback));
    }

    task_t<TMessage> async ()
    {
        auto task = task_t<TMessage> (
          [inner = std::move (_inner)] (std::function<void (result_t<TMessage>)> callback) mutable {
              inner.submit (std::move (callback));
          });
        task.start ();
        return task;
    }

    std::future<TMessage> to_future (std::string failure_message = "stream wait failed")
    {
        return _inner.to_future (std::move (failure_message));
    }

  private:
    wait_call_t<TMessage> _inner;
};

template <typename TMessage> class coroutine_expect_none_call_t
{
  public:
    explicit coroutine_expect_none_call_t (expect_none_call_t<TMessage> inner) :
        _inner (std::move (inner))
    {
    }

    coroutine_expect_none_call_t &within (std::chrono::milliseconds window)
    {
        _inner.within (window);
        return *this;
    }

    result_t<void> submit () { return _inner.submit (); }

    void submit (std::function<void (result_t<void>)> callback)
    {
        _inner.submit (std::move (callback));
    }

    task_t<void> async ()
    {
        auto task = task_t<void> (
          [inner = std::move (_inner)] (std::function<void (result_t<void>)> callback) mutable {
              inner.submit (std::move (callback));
          });
        task.start ();
        return task;
    }

  private:
    expect_none_call_t<TMessage> _inner;
};

template <typename TMessage> class coroutine_wait_for_sequence_call_t
{
  public:
    explicit coroutine_wait_for_sequence_call_t (wait_for_sequence_call_t<TMessage> inner) :
        _inner (std::move (inner))
    {
    }

    coroutine_wait_for_sequence_call_t &expect (
      std::function<bool (const TMessage &)> predicate)
    {
        _inner.expect (std::move (predicate));
        return *this;
    }

    coroutine_wait_for_sequence_call_t &timeout (std::chrono::milliseconds timeout)
    {
        _inner.timeout (timeout);
        return *this;
    }

    result_t<std::vector<TMessage>> submit () { return _inner.submit (); }

    void submit (std::function<void (result_t<std::vector<TMessage>>)> callback)
    {
        _inner.submit (std::move (callback));
    }

    task_t<std::vector<TMessage>> async ()
    {
        auto task = task_t<std::vector<TMessage>> (
          [inner = std::move (_inner)] (
            std::function<void (result_t<std::vector<TMessage>>)> callback) mutable {
              inner.submit (std::move (callback));
          });
        task.start ();
        return task;
    }

  private:
    wait_for_sequence_call_t<TMessage> _inner;
};

class coroutine_connector_t
{
  public:
    explicit coroutine_connector_t (connector_t &connector) : _connector (&connector) {}

    class lifecycle_call_t
    {
      public:
        using operation_t = std::function<void (std::function<void (result_t<void>)>)>;

        lifecycle_call_t (std::function<result_t<void> ()> submit_operation,
                          operation_t operation) :
            _submit_operation (std::move (submit_operation)), _operation (std::move (operation))
        {
        }

        result_t<void> submit ()
        {
            _last_result = _submit_operation ();
            return *_last_result;
        }

        explicit operator bool () { return static_cast<bool> (submit ()); }

        error_code_t error_code ()
        {
            if (!_last_result) {
                _last_result = _submit_operation ();
            }
            return _last_result->error_code ();
        }

        task_t<void> async ()
        {
            auto task = task_t<void> ([operation = std::move (_operation)] (
                                        std::function<void (result_t<void>)> callback) mutable {
                operation (std::move (callback));
            });
            task.start ();
            return task;
        }

      private:
        std::function<result_t<void> ()> _submit_operation;
        operation_t _operation;
        std::optional<result_t<void>> _last_result;
    };

    template <typename TMessage> coroutine_send_call_t send (const TMessage &message)
    {
        return coroutine_send_call_t (_connector->send (message));
    }

    coroutine_send_call_t send (packet_t packet)
    {
        return coroutine_send_call_t (_connector->send (std::move (packet)));
    }

    template <typename TRequest> coroutine_request_call_t request (const TRequest &request)
    {
        return coroutine_request_call_t (_connector->request (request));
    }

    coroutine_request_call_t request (packet_t packet)
    {
        return coroutine_request_call_t (_connector->request (std::move (packet)));
    }

    template <typename TMessage> coroutine_wait_call_t<TMessage> wait_for ()
    {
        return coroutine_wait_call_t<TMessage> (_connector->wait_for<TMessage> ());
    }

    template <typename TMessage>
    coroutine_wait_call_t<TMessage> wait_for (std::chrono::milliseconds timeout)
    {
        return coroutine_wait_call_t<TMessage> (_connector->wait_for<TMessage> (timeout));
    }

    template <typename TMessage> coroutine_wait_call_t<TMessage> wait_for (std::string packet_name)
    {
        return coroutine_wait_call_t<TMessage> (
          _connector->wait_for<TMessage> (std::move (packet_name)));
    }

    template <typename TMessage>
    coroutine_wait_call_t<TMessage> wait_for (std::string packet_name,
                                              std::chrono::milliseconds timeout)
    {
        return coroutine_wait_call_t<TMessage> (
          _connector->wait_for<TMessage> (std::move (packet_name), timeout));
    }

    coroutine_expect_none_call_t<packet_t> expect_none (std::string packet_name)
    {
        return coroutine_expect_none_call_t<packet_t> (
          _connector->expect_none (std::move (packet_name)));
    }

    template <typename TMessage> coroutine_expect_none_call_t<TMessage> expect_none ()
    {
        return coroutine_expect_none_call_t<TMessage> (_connector->expect_none<TMessage> ());
    }

    template <typename TMessage>
    coroutine_expect_none_call_t<TMessage> expect_none (std::string packet_name)
    {
        return coroutine_expect_none_call_t<TMessage> (
          _connector->expect_none<TMessage> (std::move (packet_name)));
    }

    coroutine_wait_for_sequence_call_t<packet_t> wait_for_sequence (std::string packet_name)
    {
        return coroutine_wait_for_sequence_call_t<packet_t> (
          _connector->wait_for_sequence (std::move (packet_name)));
    }

    template <typename TMessage>
    coroutine_wait_for_sequence_call_t<TMessage> wait_for_sequence ()
    {
        return coroutine_wait_for_sequence_call_t<TMessage> (
          _connector->wait_for_sequence<TMessage> ());
    }

    template <typename TMessage>
    coroutine_wait_for_sequence_call_t<TMessage> wait_for_sequence (std::string packet_name)
    {
        return coroutine_wait_for_sequence_call_t<TMessage> (
          _connector->wait_for_sequence<TMessage> (std::move (packet_name)));
    }

    lifecycle_call_t connect ()
    {
        return lifecycle_call_t (
          [connector = _connector] { return connector->connect (); },
          [connector = _connector] (std::function<void (result_t<void>)> callback) {
              connector->connect (std::move (callback));
          });
    }

    lifecycle_call_t close ()
    {
        return lifecycle_call_t (
          [connector = _connector] { return connector->close (); },
          [connector = _connector] (std::function<void (result_t<void>)> callback) {
              connector->close (std::move (callback));
          });
    }

  private:
    connector_t *_connector;
};

inline coroutine_connector_t coroutine (connector_t &connector)
{
    return coroutine_connector_t (connector);
}

inline coroutine_connector_t use (connector_t &connector)
{
    return coroutine (connector);
}

} // namespace zlink::stream_e2e_client
