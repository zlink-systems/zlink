/* SPDX-License-Identifier: Apache-2.0 */

namespace Zlink.HttpClient.Runtime;

/// <summary>
///     Internal description of a single request as assembled by <see cref="ZLinkHttpRequestBuilder" />.
///     Mirrors the C++ <c>http_request_t</c>. <see cref="Target" /> is the path (starting with
///     <c>/</c>) with percent-encoded query already appended.
/// </summary>
internal sealed class HttpRequestSpec
{
    public required ZLinkHttpMethod Method { get; init; }

    public required string Target { get; init; }

    public byte[]? Body { get; init; }

    public Func<byte[]?>? BodyProvider { get; init; }

    public required IReadOnlyDictionary<string, string> Headers { get; init; }

    public TimeSpan? Timeout { get; init; }

    public Action<ReadOnlyMemory<byte>>? Sink { get; init; }

    public bool IsStreaming => Sink is not null || BodyProvider is not null;
}