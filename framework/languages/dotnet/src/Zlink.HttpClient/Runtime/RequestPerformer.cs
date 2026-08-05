/* SPDX-License-Identifier: Apache-2.0 */

using System.Net;
using System.Text;
using SystemHttpClient = System.Net.Http.HttpClient;

namespace Zlink.HttpClient.Runtime;

/// <summary>
///     Performs a single logical request, driving the wrapper-owned redirect loop, cookie jar
///     integration, and same-origin <c>Authorization</c> scrubbing. Redirect/URL rules live in
///     <see cref="HttpRedirectPolicy" /> and response decoding in <see cref="ResponseBodyReader" />.
///     Mirrors the C++ <c>request_performer.cpp</c>. Native auto-redirect/auto-decompress/auto-cookie are
///     disabled on the handler (see <see cref="HttpClientRuntime" />) so these semantics match the ZLink
///     contract.
/// </summary>
internal sealed class RequestPerformer(
    HttpClientOptions options,
    CookieJar cookieJar,
    SystemHttpClient httpClient)
{
    private readonly ResponseBodyReader _bodyReader = new(options);

    public async ValueTask<RawHttpResponse> PerformAsync(HttpRequestSpec request, CancellationToken cancellationToken)
    {
        var baseUri = new Uri(options.BaseUrl);
        // GetLeftPart(Authority) keeps IPv6 literals bracketed (e.g. http://[::1]:8080), which string
        // interpolation of Host+Port would break.
        var origin = new Uri(baseUri.GetLeftPart(UriPartial.Authority));
        var combinedTarget = HttpRedirectPolicy.MakeTarget(baseUri.AbsolutePath, request.Target);
        var current = new Uri(origin, combinedTarget);
        var authorizationAllowed = HttpRedirectPolicy.SameOrigin(origin, current);
        var method = request.Method;
        var body = request.Body;
        // A streamed body provider cannot be rewound, so it is dropped once a redirect is followed
        // (kept here, not in HttpRedirectPolicy, so all body-source ownership stays in one place).
        var bodyProvider = request.BodyProvider;
        var redirectsLeft = options.FollowRedirects;

        while (true)
        {
            using var message = BuildMessage(
                method,
                body,
                bodyProvider,
                request.Headers,
                current,
                authorizationAllowed);
            var completion = request.Sink is null
                ? HttpCompletionOption.ResponseContentRead
                : HttpCompletionOption.ResponseHeadersRead;
            using var response = await httpClient
                .SendAsync(message, completion, cancellationToken)
                .ConfigureAwait(false);

            var status = (int)response.StatusCode;
            if (options.Cookies && response.Headers.TryGetValues("Set-Cookie", out var setCookies))
                foreach (var setCookie in setCookies)
                    cookieJar.Store(current.Host, setCookie);

            var location = response.Headers.TryGetValues("Location", out var locations)
                ? locations.FirstOrDefault()
                : null;
            if (options.FollowRedirects > 0 && HttpRedirectPolicy.IsRedirectStatus(status) &&
                !string.IsNullOrEmpty(location))
            {
                if (redirectsLeft == 0)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.ProtocolError,
                        "HTTP request exceeded the redirect limit");

                --redirectsLeft;
                (method, body) = HttpRedirectPolicy.RewriteForRedirect(status, method, body);
                bodyProvider = null; // consumed; never replay a non-rewindable stream on a redirect
                var next = HttpRedirectPolicy.ResolveLocation(current, location!);
                authorizationAllowed &= HttpRedirectPolicy.SameOrigin(current, next);
                current = next;
                continue;
            }

            var headers = ResponseBodyReader.CollectHeaders(response);
            if (request.Sink is not null)
            {
                await _bodyReader.StreamToSinkAsync(response, request.Sink, cancellationToken).ConfigureAwait(false);
                return new RawHttpResponse
                {
                    Status = status,
                    Headers = headers,
                    BodyBytes = []
                };
            }

            var bytes = await _bodyReader.ReadBufferedAsync(response, cancellationToken).ConfigureAwait(false);
            if (options.Compression) (bytes, headers) = _bodyReader.Decompress(bytes, headers);

            return new RawHttpResponse
            {
                Status = status,
                Headers = headers,
                BodyBytes = bytes
            };
        }
    }

    private HttpRequestMessage BuildMessage(
        ZLinkHttpMethod method,
        byte[]? body,
        Func<byte[]?>? bodyProvider,
        IReadOnlyDictionary<string, string> requestHeaders,
        Uri target,
        bool keepAuthorization)
    {
        var message = new HttpRequestMessage(ToHttpMethod(method), target)
        {
            Version = HttpVersion.Version11
        };

        var contentType = HttpHeaderLookup.Find(requestHeaders, "content-type");
        var streaming = bodyProvider is not null;
        HttpContent? content = null;
        if (bodyProvider is not null)
        {
            content = new StreamContent(new ProviderReadStream(bodyProvider));
            content.Headers.TryAddWithoutValidation("Content-Type", contentType ?? "application/octet-stream");
        }
        else if (body is not null)
        {
            content = new ByteArrayContent(body);
            if (contentType is not null) content.Headers.TryAddWithoutValidation("Content-Type", contentType);
        }

        message.Content = content;
        if (streaming) message.Headers.TransferEncodingChunked = true;

        // HttpListener (and many servers) do not emit 100-Continue; waiting for it would deadlock
        // a streamed/chunked upload. We never use the Expect/continue handshake.
        message.Headers.ExpectContinue = false;
        message.Headers.TryAddWithoutValidation("User-Agent", HttpClientVersion.UserAgent);
        message.Headers.TryAddWithoutValidation("Accept", "application/json");

        if (options.Compression) message.Headers.TryAddWithoutValidation("Accept-Encoding", "gzip, deflate");

        ApplyHeaders(message, options.Headers, keepAuthorization);
        ApplyHeaders(message, requestHeaders, keepAuthorization);

        if (options.Cookies)
        {
            var path = HttpRedirectPolicy.PathOf(target);
            var cookieHeader = cookieJar.HeaderFor(target.Host, path, target.Scheme == "https");
            if (cookieHeader.Length > 0) message.Headers.TryAddWithoutValidation("Cookie", cookieHeader);
        }

        return message;
    }

    private static void ApplyHeaders(
        HttpRequestMessage message,
        IReadOnlyDictionary<string, string> headers,
        bool keepAuthorization)
    {
        foreach (var (name, value) in headers)
        {
            if (name.Equals("content-type",
                    StringComparison.OrdinalIgnoreCase)) continue; // routed to the content above

            if (!keepAuthorization && name.Equals("authorization", StringComparison.OrdinalIgnoreCase)) continue;

            message.Headers.Remove(name);
            message.Headers.TryAddWithoutValidation(name, value);
        }
    }

    private static HttpMethod ToHttpMethod(ZLinkHttpMethod method)
    {
        return method switch
        {
            ZLinkHttpMethod.Get => HttpMethod.Get,
            ZLinkHttpMethod.Post => HttpMethod.Post,
            ZLinkHttpMethod.Put => HttpMethod.Put,
            ZLinkHttpMethod.Delete => HttpMethod.Delete,
            ZLinkHttpMethod.Patch => HttpMethod.Patch,
            ZLinkHttpMethod.Head => HttpMethod.Head,
            ZLinkHttpMethod.Options => HttpMethod.Options,
            _ => HttpMethod.Get
        };
    }

}
