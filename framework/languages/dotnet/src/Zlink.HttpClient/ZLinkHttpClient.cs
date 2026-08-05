/* SPDX-License-Identifier: Apache-2.0 */

using Zlink.HttpClient.Runtime;

namespace Zlink.HttpClient;

/// <summary>
///     ZLink-style fluent HTTP client. Wraps <c>System.Net.Http.HttpClient</c> behind a builder so
///     transport types never leak into application code. A general HTTP client; the typed-JSON path
///     (<c>Body(dto)</c> / <c>Async&lt;T&gt;()</c>) is a convenience
///     layer on top. Mirrors the C++ <c>zlink::http_client::client_t</c>.
/// </summary>
public class ZLinkHttpClient : IDisposable
{
    internal ZLinkHttpClient(HttpClientRuntime runtime)
    {
        Runtime = runtime;
    }

    internal HttpClientRuntime Runtime { get; }

    public void Dispose()
    {
        Runtime.Dispose();
    }

    /// <summary>Starts a new client builder.</summary>
    public static ZLinkHttpClientBuilder Create()
    {
        return new ZLinkHttpClientBuilder();
    }

    /// <summary>Starts a new client builder with a base URL.</summary>
    public static ZLinkHttpClientBuilder Create(string baseUrl)
    {
        return new ZLinkHttpClientBuilder().BaseUrl(baseUrl);
    }

    public virtual ZLinkHttpRequestBuilder Get(string path)
    {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.Get, path);
    }

    public virtual ZLinkHttpRequestBuilder Post(string path)
    {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.Post, path);
    }

    public virtual ZLinkHttpRequestBuilder Put(string path)
    {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.Put, path);
    }

    public virtual ZLinkHttpRequestBuilder Delete(string path)
    {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.Delete, path);
    }

    public virtual ZLinkHttpRequestBuilder Patch(string path)
    {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.Patch, path);
    }

    public virtual ZLinkHttpRequestBuilder Head(string path)
    {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.Head, path);
    }

    public virtual ZLinkHttpRequestBuilder Options(string path)
    {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.Options, path);
    }
}

/// <summary>
///     HTTP client injected into framework server code. Its request builders add the server-only
///     one-way <c>Async</c> terminator.
/// </summary>
public sealed class ZLinkHttpServerClient : ZLinkHttpClient
{
    internal ZLinkHttpServerClient(HttpClientRuntime runtime) : base(runtime)
    {
    }

    public override ZLinkHttpServerRequestBuilder Get(string path) =>
        new(this, ZLinkHttpMethod.Get, path);

    public override ZLinkHttpServerRequestBuilder Post(string path) =>
        new(this, ZLinkHttpMethod.Post, path);

    public override ZLinkHttpServerRequestBuilder Put(string path) =>
        new(this, ZLinkHttpMethod.Put, path);

    public override ZLinkHttpServerRequestBuilder Delete(string path) =>
        new(this, ZLinkHttpMethod.Delete, path);

    public override ZLinkHttpServerRequestBuilder Patch(string path) =>
        new(this, ZLinkHttpMethod.Patch, path);

    public override ZLinkHttpServerRequestBuilder Head(string path) =>
        new(this, ZLinkHttpMethod.Head, path);

    public override ZLinkHttpServerRequestBuilder Options(string path) =>
        new(this, ZLinkHttpMethod.Options, path);
}
