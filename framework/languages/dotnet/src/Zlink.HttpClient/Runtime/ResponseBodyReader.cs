/* SPDX-License-Identifier: Apache-2.0 */

using System.Net.Http.Headers;

namespace Zlink.HttpClient.Runtime;

/// <summary>
///     Reads and decodes HTTP response bodies for the wrapper: buffered read with the configured size
///     limit, streaming delivery to a sink, header collection, and wrapper-controlled gzip/deflate
///     decompression. Separated from <see cref="RequestPerformer" /> so the request/redirect flow stays
///     independent of response-decoding mechanics.
/// </summary>
internal sealed class ResponseBodyReader(HttpClientOptions options)
{
    public async ValueTask StreamToSinkAsync(
        HttpResponseMessage response,
        Action<ReadOnlyMemory<byte>> sink,
        CancellationToken cancellationToken)
    {
        await using var stream = await response.Content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
        var buffer = new byte[16384];
        long total = 0;
        int read;
        while ((read = await stream.ReadAsync(buffer, cancellationToken).ConfigureAwait(false)) > 0)
        {
            total += read;
            if (total > options.MaxResponseBodySize) throw RequestError("HTTP response exceeded the maximum body size");

            sink(new ReadOnlyMemory<byte>(buffer, 0, read));
        }
    }

    public async ValueTask<byte[]> ReadBufferedAsync(HttpResponseMessage response, CancellationToken cancellationToken)
    {
        await using var stream = await response.Content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
        using var output = new MemoryStream();
        var buffer = new byte[16384];
        int read;
        while ((read = await stream.ReadAsync(buffer, cancellationToken).ConfigureAwait(false)) > 0)
        {
            if (output.Length + read > options.MaxResponseBodySize)
                throw RequestError("HTTP response exceeded the maximum body size");

            output.Write(buffer, 0, read);
        }

        return output.ToArray();
    }

    public (byte[] Body, IReadOnlyDictionary<string, string> Headers) Decompress(
        byte[] bytes,
        IReadOnlyDictionary<string, string> headers)
    {
        var encoding = HttpHeaderLookup.Find(headers, "content-encoding");
        // An empty body (HEAD / 204 / 304) carries no payload to decode even with Content-Encoding.
        if (encoding is null || bytes.Length == 0) return (bytes, headers);

        if (encoding.Equals("gzip", StringComparison.OrdinalIgnoreCase))
            return (ResponseCompression.Gunzip(bytes, options.MaxResponseBodySize), StripEncodingHeaders(headers));

        if (encoding.Equals("deflate", StringComparison.OrdinalIgnoreCase))
            return (ResponseCompression.InflateDeflate(bytes, options.MaxResponseBodySize),
                StripEncodingHeaders(headers));

        return (bytes, headers);
    }

    public static IReadOnlyDictionary<string, string> CollectHeaders(HttpResponseMessage response)
    {
        var headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var (name, values) in EnumerateHeaders(response.Headers))
            headers.TryAdd(name, string.Join(", ", values));

        foreach (var (name, values) in EnumerateHeaders(response.Content.Headers))
            headers.TryAdd(name, string.Join(", ", values));

        return headers;
    }

    private static IEnumerable<(string Name, IEnumerable<string> Values)> EnumerateHeaders(HttpHeaders headers)
    {
        foreach (var header in headers) yield return (header.Key, header.Value);
    }

    // After decoding, drop Content-Encoding and the now-stale Content-Length (it described the
    // compressed body, not the decoded one).
    private static IReadOnlyDictionary<string, string> StripEncodingHeaders(IReadOnlyDictionary<string, string> headers)
    {
        return HttpHeaderLookup.Without(headers, "content-encoding", "content-length");
    }

    private static ZLinkFrameworkException RequestError(string message)
    {
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.CapacityExceeded,
            message,
            ZLinkRetryAdvice.DoNotRetry);
    }
}
