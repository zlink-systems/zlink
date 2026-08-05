[← 목차](README.ko.md)

# 7. 비동기와 코루틴

`submit_raw()`/`submit<T>()`는 `zlink::framework::task_t`를 돌려준다. 결과를
소비하는 방법은 세 가지다.

## coroutine 실행 켜기

기본 client는 기존 코드와 같은 blocking submit 의미를 유지한다. HTTP 대기 중 호출
스레드를 비우려면 client 구성에서 coroutine 실행을 명시한다.

```cpp
auto client = zlink::http_client::client_t::create ("http://127.0.0.1:18080")
  .coroutines ()
  .build ();
```

`.coroutines()`는 HTTP client 내부 scheduler를 사용한다. 이 scheduler는 public header에
Boost.Asio, Boost.Beast, OpenSSL runtime 타입을 드러내지 않는다.

server runtime처럼 coroutine을 다시 실행할 위치를 직접 정해야 하는 경우에는 framework
queue adapter를 resume scheduler로 주입한다.

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

필요하면 HTTP 작업을 실행하는 scheduler와 coroutine을 다시 실행하는 scheduler를
분리해서 둘 다 제공할 수 있다.

```cpp
auto client = zlink::http_client::client_t::create ("https://matchmaking.internal")
  .coroutines (http_execute_scheduler, server_resume_scheduler)
  .build ();
```

이때 execute scheduler는 HTTP 교환 작업을 실행하고 resume scheduler는 완료 후
continuation과 callback이 실행될 위치를 정한다.

## co_await (framework handler 안에서의 표준)

framework runtime/handler 코드는 `.coroutines(...)`로 구성한 client를 `co_await`로
받는다. 성공이면 값이 나오고 실패면 `framework_exception_t`가 던져진다.

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

## blocking: .result() / fetch<T>()

`task.result()`는 결과가 올 때까지 호출 스레드를 멈추고 `result_t`를 돌려준다.
`fetch<T>()`는 거기에 더해 래퍼를 풀고 실패를 예외로 바꾼다.

```cpp
auto result = client.get ("/leaderboard").submit<leaderboard_t> ().result ();
auto board = client.get ("/leaderboard").fetch<leaderboard_t> ();   // 동등 + 언래핑
```

## 콜백 submit

`submit<T>(callback)`은 완료 시 `result_t`를 콜백으로 전달한다. coroutine 실행을 켠
client에서는 HTTP 완료 뒤 resume scheduler가 정한 위치에서 typed decode와 callback이
이어진다.

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

## 어디서 무엇을 쓰나 — blocking 규칙

> **framework runtime/handler 스레드에서는 blocking 접근(`.result()`,
> `fetch<T>()`)을 쓰지 않는다.** runtime 스레드를 멈추면 같은 스레드에서 처리될
> 다른 작업까지 막힌다.

| 호출 위치 | 권장 |
|-----------|------|
| framework handler / actor / spot 코드 | `co_await submit<T>()` |
| 테스트 코드 | `fetch<T>()` 또는 `.result()` |
| client 시나리오·CLI·배치 | `fetch<T>()` 또는 `.result()` |
| 콜백 스타일이 자연스러운 glue 코드 | `submit<T>(callback)` |

## streaming callback 위치

`body_stream(provider)`의 provider와 `download(sink)`의 sink는 HTTP 작업을 실행하는
execute scheduler worker에서 호출된다. 이 callback 안에서 server handler state를 직접
건드리지 말고 필요한 경우 thread-safe queue나 server scheduler post를 사용한다.

[다음: Streaming →](08-streaming.ko.md)
