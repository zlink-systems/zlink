---
title: "Installation and the First Request · Node/TypeScript"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/http-client/02-getting-started.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Installation and the First Request

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: HTTP Client Overview](01-overview.en.md) | [Next: Making Requests](03-making-requests.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/http-client/02-getting-started.en.md) · [C#/.NET](../../../dotnet/guide/http-client/02-getting-started.en.md) · [Java](../../../java/guide/http-client/02-getting-started.en.md) · [Kotlin](../../../kotlin/guide/http-client/02-getting-started.en.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    You can add the HTTP client package to a project, receive a typed GET response through one client,
    and choose a one-shot for a single request.

The HTTP client ships separately from the server framework. A calling process needs only the HTTP client package. The tutorial below creates a client and reads one profile.

## 1. Package Reference

```bash
npm install @zlink-systems/http-client
```

Each tab shows where that language references the package. The Java and Kotlin tabs quote the tutorial's actual Gradle dependencies, while Node/TypeScript installs the public npm package.

## 2. Client Creation and the First GET Request

A builder holding the base URL and default options creates a client. A request builder is created when a method such as GET or POST is selected from the client. A typed response terminator decodes the JSON body to the requested type and returns the response envelope. An asynchronous terminator does not occupy a thread or event loop while it waits for the result.

<iframe class="zlink-diagram" src="/common/diagrams/http-client-first-request-en.html"
        title="http client first request" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/http-client-first-request-en.html" target="_blank">↗ 크게 보기</a></p>

```typescript
--8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-client-create"
--8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-first-request"
```

The code creates a client with a three-second default timeout, sends a profile GET request, and prints the player ID and nickname from the typed response body.

## 3. Result

```text
first request: p1 rookie
```

The captured runs for four languages show the same player ID and nickname. The C++ result remains reserved for the #714 fix.

## 4. A Single Request

A one-shot obtains its request builder directly from the client builder. It closes the client after completion, so it does not reuse a connection pool. A service that calls the same API repeatedly keeps a client as in the preceding section.

## Next Chapter

[Making Requests](03-making-requests.en.md) sets methods, paths, queries, headers, and per-request timeouts. [Handling Responses](05-handling-responses.en.md) chooses a response form, and [Client and Request Lifecycle](08-client-lifecycle.en.md) covers reuse and closing.

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d&&d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
