---
title: "Kotlin Coroutine Integration"
---

# Kotlin Coroutine Integration

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: Error Handling](11-error-handling.en.md)
<!-- framework-adapter-nav:end -->

!!! info "After reading this chapter"

    This chapter distinguishes HTTP suspension, cancellation boundaries, and dispatcher boundaries in a Kotlin coroutine.
    The code example comes from the Kotlin `HttpClient` tutorial; the coroutine bridge behavior is verified against the Kotlin public interface.

Kotlin extensions turn the Java HTTP client's `CompletionStage` into suspend functions. A typed response contains
status, headers, and a decoded DTO. A raw response contains status, headers, and an undecoded body. `await` waits for
a typed response, `awaitRaw` for a raw response, `fetch` for a decoded body, and `awaitDownload` for download completion.
This chapter covers why those extensions do not occupy a thread and where a coroutine resumes.

## 1. The CompletionStage bridge

<iframe class="zlink-diagram" src="/common/diagrams/http-client-kotlin-coroutines-en.html"
        title="http client kotlin coroutines" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/http-client-kotlin-coroutines-en.html" target="_blank">↗ 크게 보기</a></p>

`await`, `awaitRaw`, `fetch`, `awaitDownload`, and `yield` are suspend functions. The extensions resume the
coroutine when the `CompletionStage` completes, so no thread is occupied while an HTTP response is pending. `yield`
is the server request builder terminator that waits while retaining the current execution turn.

The tutorial below reads a typed response through the body-only suspend extension.

```kotlin title="framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt"
--8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-first-request"
```

This code calls `fetch<PlayerProfile>()` to receive the decoded `PlayerProfile` body without the response envelope.

## 2. Coroutine cancellation and HTTP cancellation

`awaitWithoutCancellingOperation` separates a coroutine wait from ownership of an HTTP operation that has already
been submitted. Cancelling the coroutine prevents that coroutine from resuming, but cancellation does not propagate
to the HTTP operation. Its retry, body reading, and client lease therefore continue under the submitted request's rules.

!!! warning "Cancellation boundary"

    Coroutine cancellation does not stop the underlying HTTP request. Timeout and response-lifetime rules establish the business boundary for stopping work.

## 3. Dispatcher-controlled resumption

After an extension completes, its continuation resumes on the calling coroutine's dispatcher. The HTTP client does
not choose a separate dispatcher. `withContext` defines the scope of a CPU-bound or framework operation that must
run on a different dispatcher.

This separates HTTP I/O waiting from the placement of follow-up work. Long-running work in a download sink occupies
the callback's execution lane, so a sink should hand work off when further processing needs a separate coroutine boundary.

## 4. runBlocking in CLI code

Handler, actor, and spot paths call the HTTP extensions directly from suspend functions. `runBlocking` belongs only
in a CLI or test that deliberately occupies its calling thread. Using it in a runtime handler turns an asynchronous
wait into an occupied-thread wait.

## 5. Related chapters

- Client defaults and the complete execution model — [Client and Request Lifecycle](08-client-lifecycle.en.md)
- Error kinds and retry decisions — [Error Handling](11-error-handling.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d&&d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
