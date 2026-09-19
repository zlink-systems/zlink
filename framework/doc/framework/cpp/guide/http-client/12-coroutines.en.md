---
title: "C++ Coroutine Integration"
---

# C++ Coroutine Integration

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: Error Handling](11-error-handling.en.md)
<!-- framework-adapter-nav:end -->

!!! info "After reading this chapter"

    This chapter distinguishes the scheduler that executes HTTP work from the scheduler that resumes a coroutine.
    The response-shape example comes from the C++ `HttpClient` tutorial; execution placement is verified against the C++ HTTP client interface.

`task_t` represents a value that completes later, and `co_await` suspends only the current coroutine until that value is ready.
A terminator is the final call that submits the request accumulated by a request builder. The HTTP client's
0.19.0 asynchronous terminators return `task_t`. [Blocking responses in the current tutorial](#3-blocking-responses-in-the-current-tutorial)
distinguishes the current 0.18.x tutorial. This chapter covers where that work runs and where
its continuation resumes. [Client and Request Lifecycle](08-client-lifecycle.en.md) covers request and response shapes.

## 1. Execution and resumption

<iframe class="zlink-diagram" src="/common/diagrams/http-client-cpp-coroutines-en.html"
        title="http client cpp coroutines" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/http-client-cpp-coroutines-en.html" target="_blank">↗ 크게 보기</a></p>

Under the 0.19.0 contract, `async_raw()`, `async<T>()`, `fetch<T>()`, and `download(sink)` register HTTP work with the selected execute scheduler.
When the response is ready, the caller continuation and any callback run on the resume scheduler. This separates
the worker that performs HTTP I/O from the worker that continues framework work.

A `body_stream` provider and a `download` sink run on the execute worker. Directly changing server handler state
from either callback can cross execution lanes. Necessary work passes through a thread-safe queue or the server
scheduler.

## 2. Coroutine scheduler selection

The `.coroutines()` setting changes only execution and resumption placement; it does not make an operation asynchronous.

| Client setting | HTTP work | Continuation and callback |
| --- | --- | --- |
| No setting | Default execute scheduler | Default resume scheduler |
| `.coroutines()` | HTTP client internal scheduler | Same internal scheduler |
| `.coroutines(resume)` | HTTP client internal scheduler | Supplied resume scheduler |
| `.coroutines(execute, resume)` | Supplied execute scheduler | Supplied resume scheduler |

`framework_resume_scheduler_t` serves as the resume scheduler when execution must return to a framework lane.
The adapter posts the continuation to the framework queue, so an HTTP worker does not execute framework state directly.
Passing a `nullptr` scheduler fails client creation or the request with `invalid_operation`.

## 3. Blocking responses in the current tutorial

The currently distributed 0.18.x tutorial uses blocking terminators in its CLI. A typed response contains status,
headers, and a decoded DTO. A raw response contains status, headers, and an undecoded body. A body-only call omits
the response envelope and returns the decoded DTO directly.

The following code waits for typed and raw responses through `submit<T>().result()` and
`submit_raw().result()`. Its 0.18.x `fetch<T>()` call completes synchronously.

```cpp title="framework/languages/cpp/tutorial/HttpClient/main.cpp"
--8<-- "framework/languages/cpp/tutorial/HttpClient/main.cpp:http-response-kinds"
```

The code prints the typed status, the raw `content-type` header, and the nickname from the body-only DTO.

## 4. The 0.19.0 asynchronous contract

Under the 0.19.0 contract, `async<T>()`, `async_raw()`, `fetch<T>()`, and `download(sink)` all return `task_t`.
Runtime code combines these asynchronous terminators with `co_await`, so the current worker does not wait for HTTP
completion. Although `fetch<T>()` returns only the body, it remains asynchronous.

[#707](https://github.com/zlink-systems/zlink/issues/707) tracks convergence between the current tutorial and the
0.19.0 names and execution model. Until #707 reaches the distributed package and tutorial, the code above represents
the current 0.18.x blocking behavior.

## 5. Blocking terminators in CLI code

`submit_raw` and `submit<T>` wait on the calling thread and return `result_t`. They are for a CLI and client
scenario where occupying that thread is acceptable. In a framework runtime execution context, they fail immediately
with `invalid_operation`.

Waiting for a blocking result in an HTTP callback or coroutine can prevent the execute worker from starting the next
HTTP operation. Runtime code combines an asynchronous terminator with `co_await`.

## 6. Related chapters

- Client defaults and the complete execution model — [Client and Request Lifecycle](08-client-lifecycle.en.md)
- Error kinds and retry decisions — [Error Handling](11-error-handling.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d&&d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
