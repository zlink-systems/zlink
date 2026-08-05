/* SPDX-License-Identifier: Apache-2.0 */

using System.Text;

namespace Zlink.HttpClient;

/// <summary>
///     Raw HTTP response with status, headers, and the buffered body as a string.
///     Header lookup is case-insensitive. Mirrors the C++ <c>raw_http_response_t</c>.
/// </summary>
public sealed class RawHttpResponse
{
    private string? _body;

    public required int Status { get; init; }

    public required IReadOnlyDictionary<string, string> Headers { get; init; }

    /// <summary>
    ///     Decoded lazily from the buffered bytes so binary responses do not pay a UTF-8
    ///     string conversion unless the text body is actually read.
    /// </summary>
    public string Body
    {
        get => _body ??= BodyBytes.Length == 0 ? string.Empty : Encoding.UTF8.GetString(BodyBytes);
        init => _body = value;
    }

    internal byte[] BodyBytes { get; init; } = [];
}

/// <summary>
///     Typed HTTP response. <see cref="Body" /> is the JSON-decoded payload; <see cref="RawBody" />
///     keeps the original response text. Mirrors the C++ <c>http_response_t&lt;T&gt;</c>.
/// </summary>
/// <typeparam name="T">The decoded body type.</typeparam>
public sealed class HttpResponse<T>
{
    public required int Status { get; init; }

    public required IReadOnlyDictionary<string, string> Headers { get; init; }

    public required T Body { get; init; }

    public required string RawBody { get; init; }
}