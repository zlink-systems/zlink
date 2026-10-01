/* SPDX-License-Identifier: Apache-2.0 */

namespace Zlink.HttpClient.Runtime;

/// <summary>
///     Immutable snapshot of the client configuration produced by <see cref="ZLinkHttpClientBuilder" />.
///     Mirrors the C++ <c>http_client_options_t</c>. The transport (redirect loop, cookie jar,
///     compression, retry) is implemented by the wrapper around <c>SocketsHttpHandler</c>; only the
///     semantics that match the ZLink contract are delegated to the native handler.
/// </summary>
internal sealed class HttpClientOptions
{
    internal static readonly TimeSpan DefaultTimeout = TimeSpan.FromMilliseconds(3000);
    internal const long DefaultMaxResponseBodySize = 16 * 1024 * 1024;
    internal const int DefaultRedirectLimit = 5;

    public IZLinkHttpExecutionScheduler? ExecutionScheduler { get; init; }

    public required string BaseUrl { get; init; }

    public TimeSpan Timeout { get; init; } = DefaultTimeout;

    public long MaxResponseBodySize { get; init; } = DefaultMaxResponseBodySize;

    public required IReadOnlyDictionary<string, string> Headers { get; init; }

    public required HttpClientCodecRegistry Codecs { get; init; }

    public string? TrustCertificateFile { get; init; }

    public (string CertificatePath, string KeyPath)? ClientCertificate { get; init; }

    public int FollowRedirects { get; init; }

    public int RetryAttempts { get; init; }

    public bool Cookies { get; init; }

    public string? Proxy { get; init; }

    public (string User, string Password)? ProxyCredentials { get; init; }

    public bool Compression { get; init; }
}
