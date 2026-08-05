/* SPDX-License-Identifier: Apache-2.0 */

using System.Text;
using System.Text.Json;
using Zlink.HttpClient.Runtime;

namespace Zlink.HttpClient;

/// <summary>
///     Fluent builder for a single request. Mirrors the C++ <c>request_builder_t</c>. Submission
///     returns a <see cref="ValueTask{T}" />; no thread is parked while the request is in flight. The
///     <c>Async</c> keeps a server Spot turn. Server-only terminators are provided by
///     <see cref="ZLinkHttpServerRequestBuilder" />.
/// </summary>
public class ZLinkHttpRequestBuilder
{
    private readonly ZLinkHttpClientBuilder? _clientFactory;
    private readonly List<KeyValuePair<string, string>> _form = new();
    private readonly Dictionary<string, string> _headers = new(StringComparer.OrdinalIgnoreCase);
    private readonly ZLinkHttpMethod _method;
    private readonly List<MultipartPart> _multipart = new();
    private readonly bool _ownsClient;
    private readonly string _path;
    private readonly List<KeyValuePair<string, string>> _query = new();
    private byte[]? _body;
    private Func<byte[]?>? _bodyProvider;
    private ZLinkHttpClient? _client;
    private bool _consumed;
    private readonly IZLinkHttpExecutionTurn? _executionTurn;
    private TimeSpan? _timeout;

    internal ZLinkHttpRequestBuilder(ZLinkHttpClient client, ZLinkHttpMethod method, string path)
    {
        _client = client;
        _executionTurn = client.Runtime.Options.ExecutionScheduler?.Capture();
        _method = method;
        _path = path;
        ValidatePath(path);
    }

    // One-shot constructor: the client is built lazily at the terminal operation and disposed
    // afterwards, so any pre-submit validation failure cannot leak an eagerly-built client.
    internal ZLinkHttpRequestBuilder(ZLinkHttpClientBuilder clientFactory, ZLinkHttpMethod method, string path)
    {
        _clientFactory = clientFactory;
        _ownsClient = true;
        _method = method;
        _path = path;
        ValidatePath(path);
    }

    private static void ValidatePath(string path)
    {
        if (path.Length == 0 || path[0] != '/')
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "HTTP request path must start with /");
    }

    public virtual ZLinkHttpRequestBuilder Header(string name, string value)
    {
        HttpClientText.RequireNonBlank(name, "HTTP request header name is required");
        _headers[name] = value;
        return this;
    }

    public virtual ZLinkHttpRequestBuilder Query(string name, string value)
    {
        HttpClientText.RequireNonBlank(name, "HTTP request query name is required");
        _query.Add(new KeyValuePair<string, string>(name, value));
        return this;
    }

    public virtual ZLinkHttpRequestBuilder Timeout(TimeSpan value)
    {
        HttpClientText.RequirePositiveTimeout(value);
        _timeout = value;
        return this;
    }

    /// <summary>Sets a typed body. JSON is the default; registered codec extensions may override it.</summary>
    public virtual ZLinkHttpRequestBuilder Body<T>(T value)
    {
        var encoded = ResolveCodecs().Encode(value, typeof(T));
        _body = encoded.Body;
        _headers["content-type"] = encoded.ContentType;
        return this;
    }

    /// <summary>Sets a raw body with an explicit content type.</summary>
    public virtual ZLinkHttpRequestBuilder Body(string content, string contentType)
    {
        if (content is null)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError, "HTTP request raw body content is required");

        HttpClientText.RequireNonBlank(contentType, "HTTP request body content type is required");
        _body = Encoding.UTF8.GetBytes(content);
        _headers["content-type"] = contentType;
        return this;
    }

    /// <summary>
    ///     Streams the request body chunk by chunk with chunked transfer-encoding; the provider
    ///     returns <c>null</c> when the body is complete. Streamed requests are excluded from retry.
    /// </summary>
    public virtual ZLinkHttpRequestBuilder BodyStream(Func<byte[]?> provider, string contentType)
    {
        ArgumentNullException.ThrowIfNull(provider);
        HttpClientText.RequireNonBlank(contentType, "HTTP request body content type is required");
        _bodyProvider = provider;
        _headers["content-type"] = contentType;
        return this;
    }

    public virtual ZLinkHttpRequestBuilder Form(string name, string value)
    {
        HttpClientText.RequireNonBlank(name, "HTTP request form field name is required");
        _form.Add(new KeyValuePair<string, string>(name, value));
        return this;
    }

    public virtual ZLinkHttpRequestBuilder Multipart(string name, string value)
    {
        HttpClientText.RequireNonBlank(name, "HTTP request multipart field name is required");
        _multipart.Add(new MultipartPart(name, string.Empty, value, string.Empty));
        return this;
    }

    public virtual ZLinkHttpRequestBuilder MultipartFile(string name, string filename, string content, string contentType)
    {
        HttpClientText.RequireNonBlank(name, "HTTP request multipart field name is required");
        HttpClientText.RequireNonBlank(filename, "HTTP request multipart filename is required");
        HttpClientText.RequireNonBlank(contentType, "HTTP request multipart content type is required");
        _multipart.Add(new MultipartPart(name, filename, content, contentType));
        return this;
    }

    // Resolves the client to run on, building a one-shot client lazily. A one-shot request builder is
    // single-use so its lazily-built client is disposed exactly once.
    private ZLinkHttpClient ResolveClient()
    {
        if (_ownsClient && _consumed)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "A one-shot HTTP request can only be submitted once");

        _consumed = true;
        return _client ??= _clientFactory!.Build();
    }

    private void ReleaseIfOwned(ZLinkHttpClient client)
    {
        if (_ownsClient) client.Dispose();
    }

    /// <summary>Submits the request and returns the raw response.</summary>
    public async ValueTask<RawHttpResponse> AsyncRaw(CancellationToken cancellationToken = default)
    {
        var client = ResolveClient();
        try
        {
            return await client.Runtime.ExecuteAsync(MakeRequest(null), cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            ReleaseIfOwned(client);
        }
    }

    /// <summary>
    ///     Streams the response body to <paramref name="sink" /> chunk by chunk instead of buffering it;
    ///     the returned response carries status and headers with an empty body. Chunks are delivered as
    ///     received (no content-encoding decompression).
    /// </summary>
    public async ValueTask<RawHttpResponse> DownloadAsync(Action<ReadOnlyMemory<byte>> sink,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(sink);
        var client = ResolveClient();
        try
        {
            return await client.Runtime.ExecuteAsync(MakeRequest(sink), cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            ReleaseIfOwned(client);
        }
    }

    /// <summary>Submits the request and decodes the JSON body to <typeparamref name="T" />.</summary>
    public ValueTask<HttpResponse<T>> Async<T>(CancellationToken cancellationToken = default)
    {
        return ExecuteTypedAsync<T>(cancellationToken);
    }

    /// <summary>Submits the request and returns the decoded response body.</summary>
    public async ValueTask<T> Fetch<T>(
        CancellationToken cancellationToken = default)
    {
        var response = await ExecuteTypedAsync<T>(cancellationToken)
            .ConfigureAwait(false);
        return response.Body;
    }

    /// <summary>
    ///     Starts the request and reports completion through a callback. Exactly one of
    ///     <c>error</c> and <c>response</c> is non-null. A server callback is queued as a new turn on
    ///     the execution line captured when this request was created.
    /// </summary>
    public void Async<T>(ZLinkHttpCallback<T> callback, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(callback);
        _ = CompleteCallbackAsync(ExecuteTypedAsync<T>(cancellationToken), callback);
    }

    protected async ValueTask<HttpResponse<T>> ExecuteTypedAsync<T>(CancellationToken cancellationToken)
    {
        var codecs = ResolveCodecs();
        var raw = await AsyncRaw(cancellationToken).ConfigureAwait(false);
        if (raw.Status >= 400)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                $"HTTP request failed with status {raw.Status}");

        T body;
        try
        {
            if (raw.BodyBytes.Length == 0)
            {
                body = default!;
            }
            else
            {
                var contentType = HttpHeaderLookup.Find(raw.Headers, "content-type");
                body = (T?)codecs.Decode(
                           raw.BodyBytes,
                           typeof(T),
                           contentType)
                       ?? throw new InvalidOperationException("HTTP response body decoded to null");
            }
        }
        catch (Exception ex) when (ex is InvalidOperationException or NotSupportedException or JsonException)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError, ex.Message, innerException: ex);
        }

        return new HttpResponse<T>
        {
            Status = raw.Status,
            Headers = raw.Headers,
            Body = body,
            RawBody = raw.Body
        };
    }

    protected IZLinkHttpExecutionTurn RequireExecutionTurn(string operation)
    {
        return _executionTurn
               ?? throw new ZLinkFrameworkException(
                   ZLinkFrameworkErrorKind.ProtocolError,
                   $"HTTP {operation} can only run inside a framework execution turn.");
    }

    protected async Task ObserveSubmissionAsync(
        IZLinkHttpExecutionTurn turn,
        CancellationToken cancellationToken)
    {
        try
        {
            _ = await AsyncRaw(cancellationToken).ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            turn.ReportError(exception);
        }
    }

    private async Task CompleteCallbackAsync<T>(
        ValueTask<HttpResponse<T>> pending,
        ZLinkHttpCallback<T> callback)
    {
        Exception? error = null;
        HttpResponse<T>? response = null;
        try
        {
            response = await pending.ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            error = exception;
        }

        void Complete()
        {
            try
            {
                callback(error, response);
            }
            catch (Exception exception)
            {
                _executionTurn?.ReportError(exception);
            }
        }

        if (_executionTurn is null)
            Complete();
        else
            try
            {
                _executionTurn.Post(Complete);
            }
            catch (Exception exception)
            {
                _executionTurn.ReportError(exception);
            }
    }

    private HttpRequestSpec MakeRequest(Action<ReadOnlyMemory<byte>>? sink)
    {
        var (body, headers) = ResolveBodyAndHeaders();
        return new HttpRequestSpec
        {
            Method = _method,
            Target = ResolveTarget(),
            Body = body,
            BodyProvider = _bodyProvider,
            Headers = headers,
            Timeout = _timeout,
            Sink = sink
        };
    }

    private string ResolveTarget()
    {
        if (_query.Count == 0) return _path;

        var target = new StringBuilder(_path);
        var separator = _path.Contains('?') ? '&' : '?';
        foreach (var (name, value) in _query)
        {
            target.Append(separator)
                .Append(HttpClientText.PercentEncode(name))
                .Append('=')
                .Append(HttpClientText.PercentEncode(value));
            separator = '&';
        }

        return target.ToString();
    }

    private HttpClientCodecRegistry ResolveCodecs()
    {
        return _client?.Runtime.Options.Codecs
               ?? _clientFactory?.CodecRegistry
               ?? throw new InvalidOperationException("HTTP client is not configured.");
    }

    private (byte[]? Body, IReadOnlyDictionary<string, string> Headers) ResolveBodyAndHeaders()
    {
        if (CountBodySources() > 1)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "HTTP request accepts a single body source: body, body_stream, form, or multipart");

        var headers = new Dictionary<string, string>(_headers, StringComparer.OrdinalIgnoreCase);
        if (_body is not null) return (_body, headers);

        if (_form.Count > 0)
        {
            headers["content-type"] = "application/x-www-form-urlencoded";
            return (Encoding.UTF8.GetBytes(EncodeFormBody()), headers);
        }

        if (_multipart.Count > 0)
        {
            var boundary = HttpClientText.MakeMultipartBoundary();
            headers["content-type"] = "multipart/form-data; boundary=" + boundary;
            return (Encoding.UTF8.GetBytes(EncodeMultipartBody(boundary)), headers);
        }

        return (null, headers);
    }

    private int CountBodySources()
    {
        return (_body is not null ? 1 : 0) + (_bodyProvider is not null ? 1 : 0)
                                           + (_form.Count > 0 ? 1 : 0) + (_multipart.Count > 0 ? 1 : 0);
    }

    private string EncodeFormBody()
    {
        var encoded = new StringBuilder();
        foreach (var (name, value) in _form)
        {
            if (encoded.Length > 0) encoded.Append('&');

            encoded.Append(HttpClientText.PercentEncode(name))
                .Append('=')
                .Append(HttpClientText.PercentEncode(value));
        }

        return encoded.ToString();
    }

    private string EncodeMultipartBody(string boundary)
    {
        var encoded = new StringBuilder();
        foreach (var part in _multipart)
        {
            encoded.Append("--").Append(boundary).Append("\r\n");
            encoded.Append("Content-Disposition: form-data; name=\"").Append(part.Name).Append('"');
            if (part.Filename.Length > 0) encoded.Append("; filename=\"").Append(part.Filename).Append('"');

            encoded.Append("\r\n");
            if (part.ContentType.Length > 0) encoded.Append("Content-Type: ").Append(part.ContentType).Append("\r\n");

            encoded.Append("\r\n").Append(part.Content).Append("\r\n");
        }

        encoded.Append("--").Append(boundary).Append("--\r\n");
        return encoded.ToString();
    }

    private readonly record struct MultipartPart(string Name, string Filename, string Content, string ContentType);
}

/// <summary>
///     Callback completion path for callers that do not use an awaitable. Exactly one argument is
///     non-null.
/// </summary>
public delegate void ZLinkHttpCallback<T>(Exception? error, HttpResponse<T>? response);

/// <summary>Request builder used by framework server clients.</summary>
public sealed class ZLinkHttpServerRequestBuilder : ZLinkHttpRequestBuilder
{
    internal ZLinkHttpServerRequestBuilder(
        ZLinkHttpServerClient client,
        ZLinkHttpMethod method,
        string path) : base(client, method, path)
    {
    }

    public override ZLinkHttpServerRequestBuilder Header(string name, string value)
    {
        base.Header(name, value);
        return this;
    }

    public override ZLinkHttpServerRequestBuilder Query(string name, string value)
    {
        base.Query(name, value);
        return this;
    }

    public override ZLinkHttpServerRequestBuilder Timeout(TimeSpan value)
    {
        base.Timeout(value);
        return this;
    }

    public override ZLinkHttpServerRequestBuilder Body<T>(T value)
    {
        base.Body(value);
        return this;
    }

    public override ZLinkHttpServerRequestBuilder Body(string content, string contentType)
    {
        base.Body(content, contentType);
        return this;
    }

    public override ZLinkHttpServerRequestBuilder BodyStream(Func<byte[]?> provider, string contentType)
    {
        base.BodyStream(provider, contentType);
        return this;
    }

    public override ZLinkHttpServerRequestBuilder Form(string name, string value)
    {
        base.Form(name, value);
        return this;
    }

    public override ZLinkHttpServerRequestBuilder Multipart(string name, string value)
    {
        base.Multipart(name, value);
        return this;
    }

    public override ZLinkHttpServerRequestBuilder MultipartFile(
        string name,
        string filename,
        string content,
        string contentType)
    {
        base.MultipartFile(name, filename, content, contentType);
        return this;
    }

    /// <summary>
    ///     Starts a one-way server request. Transport and response failures are reported through the
    ///     framework runtime error boundary.
    /// </summary>
    public ValueTask Async(CancellationToken cancellationToken = default)
    {
        var turn = RequireExecutionTurn("Async");
        return new ValueTask(ObserveSubmissionAsync(turn, cancellationToken));
    }

    /// <summary>
    ///     Submits the request, releases the shared Spot gate while the response is outstanding, and
    ///     resumes the continuation in a new turn on the same execution line. Available where the
    ///     framework grants gate release, which is a SpotWide User Spot or an Instance Spot.
    /// </summary>
    public ValueTask<HttpResponse<T>> Yield<T>(CancellationToken cancellationToken = default)
    {
        var turn = RequireExecutionTurn("Yield");
        return turn.YieldAsync(Async<T>, cancellationToken);
    }
}
