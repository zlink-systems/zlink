/* SPDX-License-Identifier: Apache-2.0 */

using Zlink.Framework.Contracts.Codecs;
using Zlink.HttpClient.Runtime;

namespace Zlink.HttpClient;

/// <summary>
///     Fluent builder for <see cref="ZLinkHttpClient" />. Mirrors the C++ <c>client_builder_t</c>.
///     One-shot verb shortcuts build the client on demand so <see cref="Build" /> can be omitted for
///     single requests.
/// </summary>
public sealed class ZLinkHttpClientBuilder
{
    private readonly Dictionary<string, string> _headers = new(StringComparer.OrdinalIgnoreCase);
    private string _baseUrl = string.Empty;
    private (string CertificatePath, string KeyPath)? _clientCertificate;
    private bool _compression;
    private bool _cookies;
    private int _followRedirects;
    private long _maxResponseBodySize = 16 * 1024 * 1024;
    private string? _proxy;
    private (string User, string Password)? _proxyCredentials;
    private int _retryAttempts;
    private TimeSpan _timeout = TimeSpan.FromMilliseconds(3000);
    private string? _trustCertificateFile;

    internal HttpClientCodecRegistry CodecRegistry { get; } = new();

    public ZLinkHttpClientBuilder BaseUrl(string value)
    {
        ValidateBaseUrl(value);
        _baseUrl = value;
        return this;
    }

    public ZLinkHttpClientBuilder Codecs(Action<IZLinkCodecRegistryBuilder> configure)
    {
        ArgumentNullException.ThrowIfNull(configure);
        configure(CodecRegistry);
        return this;
    }

    public ZLinkHttpClientBuilder Timeout(TimeSpan value)
    {
        HttpClientText.RequirePositiveTimeout(value);
        _timeout = value;
        return this;
    }

    public ZLinkHttpClientBuilder DefaultHeader(string name, string value)
    {
        HttpClientText.RequireNonBlank(name, "HTTP client default header name is required");
        _headers[name] = value;
        return this;
    }

    public ZLinkHttpClientBuilder BasicAuth(string user, string password)
    {
        HttpClientText.RequireNonBlank(user, "HTTP client basic auth user is required");
        _headers["authorization"] = HttpClientText.BasicAuthorization(user, password);
        return this;
    }

    public ZLinkHttpClientBuilder BearerToken(string token)
    {
        HttpClientText.RequireNonBlank(token, "HTTP client bearer token is required");
        _headers["authorization"] = "Bearer " + token;
        return this;
    }

    public ZLinkHttpClientBuilder MaxResponseBodySize(long bytes)
    {
        if (bytes <= 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "HTTP client max response body size must be greater than zero");

        _maxResponseBodySize = bytes;
        return this;
    }

    public ZLinkHttpClientBuilder TrustCertificateFile(string path)
    {
        HttpClientText.RequireNonBlank(path, "HTTP client trust certificate file is required");
        _trustCertificateFile = path;
        return this;
    }

    public ZLinkHttpClientBuilder ClientCertificateFile(string certificatePath, string keyPath)
    {
        HttpClientText.RequireNonBlank(certificatePath, "HTTP client certificate file is required");
        HttpClientText.RequireNonBlank(keyPath, "HTTP client certificate key file is required");
        _clientCertificate = (certificatePath, keyPath);
        return this;
    }

    public ZLinkHttpClientBuilder FollowRedirects(int maxRedirects = 5)
    {
        if (maxRedirects <= 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "HTTP client follow_redirects must be greater than zero");

        _followRedirects = maxRedirects;
        return this;
    }

    public ZLinkHttpClientBuilder Retry(int attempts)
    {
        if (attempts <= 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "HTTP client retry attempts must be greater than zero");

        _retryAttempts = attempts;
        return this;
    }

    public ZLinkHttpClientBuilder Cookies()
    {
        _cookies = true;
        return this;
    }

    public ZLinkHttpClientBuilder Proxy(string url)
    {
        HttpClientText.RequireNonBlank(url, "HTTP client proxy url is required");
        if (!url.StartsWith("http://", StringComparison.Ordinal))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "HTTP client proxy url must start with http://");

        _proxy = url;
        return this;
    }

    public ZLinkHttpClientBuilder ProxyBasicAuth(string user, string password)
    {
        HttpClientText.RequireNonBlank(user, "HTTP client proxy auth user is required");
        _proxyCredentials = (user, password);
        return this;
    }

    public ZLinkHttpClientBuilder Compression()
    {
        _compression = true;
        return this;
    }

    /// <summary>Builds an immutable client. Validation mirrors the C++ <c>build()</c>.</summary>
    public ZLinkHttpClient Build()
    {
        HttpClientText.RequireNonBlank(_baseUrl, "HTTP client base_url is required");
        HttpClientText.RequirePositiveTimeout(_timeout);
        ValidateBaseUrl(_baseUrl);

        return new ZLinkHttpClient(new HttpClientRuntime(BuildOptions(null)));
    }

    /// <summary>Builds the server client surface with a framework execution scheduler.</summary>
    public ZLinkHttpServerClient BuildServer(IZLinkHttpExecutionScheduler scheduler)
    {
        ArgumentNullException.ThrowIfNull(scheduler);
        HttpClientText.RequireNonBlank(_baseUrl, "HTTP client base_url is required");
        HttpClientText.RequirePositiveTimeout(_timeout);
        ValidateBaseUrl(_baseUrl);

        return new ZLinkHttpServerClient(new HttpClientRuntime(BuildOptions(scheduler)));
    }

    private static void ValidateBaseUrl(string value)
    {
        HttpClientText.RequireNonBlank(value, "HTTP client base_url is required");
        if (!Uri.TryCreate(value, UriKind.Absolute, out var uri)
            || (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps)
            || string.IsNullOrWhiteSpace(uri.Host))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "HTTP client base_url must be an absolute http or https URL");
    }

    private HttpClientOptions BuildOptions(IZLinkHttpExecutionScheduler? scheduler)
    {
        return new HttpClientOptions
        {
            ExecutionScheduler = scheduler,
            BaseUrl = _baseUrl,
            Timeout = _timeout,
            MaxResponseBodySize = _maxResponseBodySize,
            Headers = new Dictionary<string, string>(_headers, StringComparer.OrdinalIgnoreCase),
            Codecs = CodecRegistry.Snapshot(),
            TrustCertificateFile = _trustCertificateFile,
            ClientCertificate = _clientCertificate,
            FollowRedirects = _followRedirects,
            RetryAttempts = _retryAttempts,
            Cookies = _cookies,
            Proxy = _proxy,
            ProxyCredentials = _proxyCredentials,
            Compression = _compression
        };

    }

    public ZLinkHttpRequestBuilder Get(string path)
    {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.Get, path);
    }

    public ZLinkHttpRequestBuilder Post(string path)
    {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.Post, path);
    }

    public ZLinkHttpRequestBuilder Put(string path)
    {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.Put, path);
    }

    public ZLinkHttpRequestBuilder Delete(string path)
    {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.Delete, path);
    }

    public ZLinkHttpRequestBuilder Patch(string path)
    {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.Patch, path);
    }

    public ZLinkHttpRequestBuilder Head(string path)
    {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.Head, path);
    }

    public ZLinkHttpRequestBuilder Options(string path)
    {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.Options, path);
    }
}
