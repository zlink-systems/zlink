/* SPDX-License-Identifier: Apache-2.0 */

using System.Text;

namespace Zlink.HttpClient.Runtime;

/// <summary>
///     Wrapper-owned cookie jar that mirrors the deliberately narrow C++ <c>cookie_jar.cpp</c>
///     semantics so behaviour matches across languages: host-exact storage (no <c>Domain</c>),
///     default <c>Path=/</c>, only <c>Path</c>/<c>Secure</c>/<c>Max-Age&lt;=0</c> attributes honoured
///     (<c>Domain</c>/<c>Expires</c> ignored), at most 128 cookies per host, and secure cookies sent
///     only on secure requests. The native <c>CookieContainer</c> implements full RFC 6265 and would
///     diverge on default path/domain/expiry, so it is disabled in favour of this jar.
/// </summary>
internal sealed class CookieJar
{
    private const int MaxCookiesPerHost = 128;
    private readonly List<Cookie> _cookies = new();

    private readonly object _lock = new();

    /// <summary>Stores one <c>Set-Cookie</c> header value for <paramref name="host" />.</summary>
    public void Store(string host, string setCookieHeader)
    {
        var remaining = setCookieHeader.AsSpan();
        var pair = TakeSegment(ref remaining);
        if (pair is null) return;

        var equals = pair.IndexOf('=');
        if (equals < 0) return;

        var name = pair[..equals].Trim();
        var value = pair[(equals + 1)..].Trim();
        if (name.Length == 0) return;

        var path = "/";
        var secure = false;
        var expired = false;
        while (TakeSegment(ref remaining) is { } attribute)
        {
            var attrEquals = attribute.IndexOf('=');
            var attrName = attrEquals < 0 ? attribute : attribute[..attrEquals].Trim();
            var attrValue = attrEquals < 0 ? string.Empty : attribute[(attrEquals + 1)..].Trim();
            if (attrName.Equals("path", StringComparison.OrdinalIgnoreCase) && attrValue.Length > 0)
                path = attrValue;
            else if (attrName.Equals("secure", StringComparison.OrdinalIgnoreCase))
                secure = true;
            else if (attrName.Equals("max-age", StringComparison.OrdinalIgnoreCase)
                     && long.TryParse(attrValue, out var maxAge))
                expired = maxAge <= 0;
        }

        lock (_lock)
        {
            _cookies.RemoveAll(existing =>
                existing.Host == host && existing.Name == name && existing.Path == path);
            if (expired) return;

            _cookies.Add(new Cookie(host, name, value, path, secure));
            var countForHost = _cookies.Count(stored => stored.Host == host);
            while (countForHost > MaxCookiesPerHost)
            {
                var oldest = _cookies.FindIndex(stored => stored.Host == host);
                if (oldest < 0) break;

                _cookies.RemoveAt(oldest);
                --countForHost;
            }
        }
    }

    /// <summary>Builds the <c>Cookie</c> header for a request, or empty string if none apply.</summary>
    public string HeaderFor(string host, string path, bool secure)
    {
        lock (_lock)
        {
            var header = new StringBuilder();
            foreach (var cookie in _cookies)
            {
                if (cookie.Host != host || (cookie.Secure && !secure)
                                        || !PathMatches(path, cookie.Path))
                    continue;

                if (header.Length > 0) header.Append("; ");

                header.Append(cookie.Name).Append('=').Append(cookie.Value);
            }

            return header.ToString();
        }
    }

    private static bool PathMatches(string requestPath, string cookiePath)
    {
        if (cookiePath.Length == 0 || cookiePath == "/") return true;

        if (requestPath == cookiePath) return true;

        if (!requestPath.StartsWith(cookiePath, StringComparison.Ordinal)) return false;

        return cookiePath[^1] == '/' || requestPath[cookiePath.Length] == '/';
    }

    private static string? TakeSegment(ref ReadOnlySpan<char> remaining)
    {
        if (remaining.IsEmpty) return null;

        var semicolon = remaining.IndexOf(';');
        var segment = (semicolon < 0 ? remaining : remaining[..semicolon]).Trim();
        remaining = semicolon < 0 ? ReadOnlySpan<char>.Empty : remaining[(semicolon + 1)..];
        return segment.ToString();
    }

    private sealed record Cookie(string Host, string Name, string Value, string Path, bool Secure);
}