[← Table Of Contents](README.en.md)

# 7. Async And Coroutines

`submit_raw()`/`submit<T>()` return a `zlink::framework::task_t`. There are three ways to consume
the result.

## Turning On Coroutine Execution

By default, the client keeps the same blocking submit semantics as existing code. To free up the
calling thread while waiting on HTTP, specify coroutine execution in the client configuration.

```cpp
auto client = zlink::http_client::client_t::create ("http://127.0.0.1:18080")
  .coroutines ()
  .build ();
```

`.coroutines()` uses the HTTP client's internal scheduler. This scheduler does not reveal
Boost.Asio, Boost.Beast, or OpenSSL runtime types in the public header.

For a case like a server runtime, where you need to directly decide where the coroutine resumes,
inject a framework queue adapter as the resume scheduler.

```cpp
auto server_resume_scheduler =
  std::make_shared<zlink::http_client::framework_resume_scheduler_t> (
    [&server_queue] (std::function<void ()> continuation) {
        server_queue.post (std::move (continuation));
    });

auto client = zlink::http_client::client_t::create ("https://matchmaking.internal")
  .coroutines (server_resume_scheduler)
  .build ();
```

If needed, you can separate the scheduler that executes the HTTP work from the scheduler that
resumes the coroutine, and provide both.

```cpp
auto client = zlink::http_client::client_t::create ("https://matchmaking.internal")
  .coroutines (http_execute_scheduler, server_resume_scheduler)
  .build ();
```

Here, the execute scheduler runs the HTTP exchange work, and the resume scheduler decides where the
continuation and callback run after completion.

## co_await (The Standard Inside A Framework Handler)

Framework runtime/handler code `co_await`s a client configured with `.coroutines(...)`. On success
you get the value; on failure, `framework_exception_t` is thrown.

```cpp
zlink::framework::task_t<void>
notify_match_result (zlink::http_client::client_t &client, const match_result_t &result)
{
    auto response = co_await client.post ("/matches/" + result.match_id + "/result")
                      .body (result)
                      .submit<ack_res_t> ();
    // response: http_response_t<ack_res_t>
    if (response.body.accepted) {
        co_return;
    }
    throw zlink::framework::framework_exception_t (
      zlink::framework::framework_error_kind_t::request_failed,
      "match result was not accepted");
}
```

## Blocking: .result() / fetch<T>()

`task.result()` stops the calling thread until the result arrives, returning a `result_t`.
`fetch<T>()` additionally unwraps the wrapper and turns a failure into an exception.

```cpp
auto result = client.get ("/leaderboard").submit<leaderboard_t> ().result ();
auto board = client.get ("/leaderboard").fetch<leaderboard_t> ();   // equivalent + unwrapped
```

## Callback Submit

`submit<T>(callback)` delivers the `result_t` to the callback on completion. On a client with
coroutine execution turned on, after the HTTP completes, typed decoding and the callback continue at
the location the resume scheduler decides.

```cpp
client.post ("/games")
  .body (create_game_http_req_t{.game_name = "ranked-match-0611"})
  .submit<create_game_http_res_t> ([] (const auto &result) {
      if (!result) {
          log_error (result.error ()->what ());
          return;
      }
      on_game_created (result.value ().body);
  });
```

## What To Use Where — The Blocking Rule

> **Don't use a blocking access (`.result()`, `fetch<T>()`) on a framework runtime/handler thread.**
> Stopping the runtime thread also blocks other work meant to be processed on that same thread.

| Call Site | Recommended |
|-----------|------|
| framework handler / actor / spot code | `co_await submit<T>()` |
| test code | `fetch<T>()` or `.result()` |
| client scenario/CLI/batch | `fetch<T>()` or `.result()` |
| glue code where a callback style is natural | `submit<T>(callback)` |

## Streaming Callback Location

`body_stream(provider)`'s provider and `download(sink)`'s sink are called on the execute scheduler
worker running the HTTP work. Don't touch server handler state directly inside this callback — use
a thread-safe queue or a server scheduler post if needed.

[Next: Streaming →](08-streaming.en.md)
