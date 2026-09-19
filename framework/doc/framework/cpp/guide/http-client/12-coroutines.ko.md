---
title: "C++ 코루틴 통합"
---

# C++ 코루틴 통합

<!-- framework-adapter-nav:start -->
[목차](README.ko.md) | [이전: 오류 처리](11-error-handling.ko.md)
<!-- framework-adapter-nav:end -->

!!! info "이 장을 읽고 나면"

    HTTP 작업을 실행할 scheduler와 coroutine을 재개할 scheduler를 구분해 설정할 수 있다.
    응답 형태 예제는 C++ `HttpClient` tutorial에서, 실행 위치의 설명은 HTTP client의 C++ 공개 인터페이스에서 확인했다.

`task_t`는 나중에 완료될 값을 나타내고 `co_await`는 그 값이 준비될 때까지 현재 coroutine만 중단한다.
request builder가 모은 요청을 제출하는 마지막 호출을 종결자(terminator)라고 한다. HTTP client의
0.19.0 비동기 종결자는 `task_t`를 반환한다. 현재 0.18.x tutorial과의 차이는
[현재 tutorial의 blocking 응답](#3-현재-tutorial의-blocking-응답)에서 구분한다. 이 장은 그 작업이 실행되는 위치와
continuation이 다시 시작되는 위치를 다룬다. 요청과 응답의 종류는 [Client와 요청의 생애](08-client-lifecycle.ko.md)에서 다룬다.

## 1. 작업 실행과 재개를 구분한다

<iframe class="zlink-diagram" src="/common/diagrams/http-client-cpp-coroutines.html"
        title="http client cpp coroutines" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/http-client-cpp-coroutines.html" target="_blank">↗ 크게 보기</a></p>

0.19.0 계약의 `async_raw()`·`async<T>()`·`fetch<T>()`·`download(sink)`는 선택된 execute scheduler에 HTTP 작업을 등록한다.
응답이 준비되면 caller의 continuation과 callback은 resume scheduler에서 다시 실행된다. 따라서
HTTP I/O를 수행하는 worker와 framework 작업을 계속할 worker를 분리할 수 있다.

`body_stream` provider와 `download` sink는 execute worker에서 호출된다. 이 callback에서 server
handler 상태를 직접 변경하면 실행 줄이 달라질 수 있다. 필요한 작업은 thread-safe queue나 server
scheduler를 통해 넘긴다.

## 2. Coroutine scheduler를 고른다

`.coroutines()` 설정은 비동기 여부를 바꾸지 않고 execute와 resume 위치만 정한다.

| Client 설정 | HTTP 작업 | Continuation과 callback |
| --- | --- | --- |
| 설정 없음 | 기본 execute scheduler | 기본 resume scheduler |
| `.coroutines()` | HTTP client 내부 scheduler | 같은 내부 scheduler |
| `.coroutines(resume)` | HTTP client 내부 scheduler | 지정한 resume scheduler |
| `.coroutines(execute, resume)` | 지정한 execute scheduler | 지정한 resume scheduler |

framework 실행 줄에 복귀해야 하면 `framework_resume_scheduler_t`를 resume scheduler로 사용한다.
이 adapter는 framework queue에 continuation을 게시하므로, HTTP worker가 framework 상태를 직접 실행하지 않는다.
`nullptr` scheduler를 전달하면 client 생성 또는 요청이 `invalid_operation`으로 실패한다.

## 3. 현재 tutorial의 blocking 응답

현재 배포된 0.18.x tutorial은 CLI에서 blocking 종결자를 사용한다. typed 응답은 status·header와
decode한 DTO를 함께 담고, raw 응답은 status·header와 decode하지 않은 body를 담는다. body만 받는
호출은 응답 봉투를 제외하고 decode한 DTO를 직접 반환한다.

다음 코드는 typed 응답과 raw 응답을 `submit<T>().result()`·`submit_raw().result()`로 기다리고,
body만 받는 호출은 현재 0.18.x의 `fetch<T>()`로 동기 완료한다.

```cpp title="framework/languages/cpp/tutorial/HttpClient/main.cpp"
--8<-- "framework/languages/cpp/tutorial/HttpClient/main.cpp:http-response-kinds"
```

이 코드는 typed 응답의 status, raw 응답의 `content-type` header, body만 받은 DTO의 nickname을 출력한다.

## 4. 0.19.0 비동기 계약

0.19.0 계약에서는 `async<T>()`·`async_raw()`·`fetch<T>()`·`download(sink)`가 모두 `task_t`를 반환한다.
runtime code는 이 비동기 종결자를 `co_await`와 결합하므로 현재 worker가 HTTP 완료를 기다리지 않는다.
`fetch<T>()`도 body를 직접 반환하지만 비동기 작업이다.

현재 tutorial과 0.19.0 계약의 이름·실행 방식 차이는
[#707](https://github.com/zlink-systems/zlink/issues/707)에서 수렴한다. #707이 배포 패키지와 tutorial에
반영되기 전까지 위 코드는 현재 0.18.x의 blocking 동작을 나타낸다.

## 5. Blocking 종결자는 CLI에만 둔다

`submit_raw`와 `submit<T>`는 호출 thread에서 완료될 때까지 기다리고 `result_t`를 반환한다. 이 경로는
CLI와 client scenario처럼 호출 thread를 점유해도 되는 곳만을 위한 것이다. framework runtime 실행 문맥에서
호출하면 `invalid_operation`으로 즉시 실패한다.

HTTP callback 또는 coroutine 안에서 blocking 결과를 기다리면 execute worker가 다음 HTTP 작업을 시작하지 못할 수 있다.
runtime code에서는 비동기 종결자와 `co_await`를 결합한다.

## 6. 다음 장

- client 기본값과 실행 모델 전체 — [Client와 요청의 생애](08-client-lifecycle.ko.md)
- 오류 kind와 재시도 판단 — [오류 처리](11-error-handling.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d&&d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
