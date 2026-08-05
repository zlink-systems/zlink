/* SPDX-License-Identifier: Apache-2.0 */

namespace Zlink.HttpClient.Runtime;

/// <summary>
///     Stateless redirect and target-URL rules for the wrapper-owned redirect loop. Keeping these in one
///     place mirrors the C++ <c>url.cpp</c> helpers and isolates the redirect contract (allowed statuses,
///     origin comparison, location resolution, method rewrite) from the request flow.
/// </summary>
internal static class HttpRedirectPolicy
{
    /// <summary>Combines the base URL path prefix with the request target.</summary>
    public static string MakeTarget(string prefix, string path)
    {
        if (prefix.Length == 0 || prefix == "/") return path;

        return prefix[^1] == '/' ? prefix[..^1] + path : prefix + path;
    }

    public static bool SameOrigin(Uri left, Uri right)
    {
        return string.Equals(left.Scheme, right.Scheme, StringComparison.OrdinalIgnoreCase)
               && string.Equals(left.Host, right.Host, StringComparison.OrdinalIgnoreCase)
               && left.Port == right.Port;
    }

    public static bool IsRedirectStatus(int status)
    {
        return status is 301 or 302 or 303 or 307 or 308;
    }

    /// <summary>The request path without query, used for cookie path matching.</summary>
    public static string PathOf(Uri uri)
    {
        var target = uri.PathAndQuery;
        var query = target.IndexOf('?');
        return query < 0 ? target : target[..query];
    }

    /// <summary>
    ///     Applies the method/body rewrite for a followed redirect: 303, and 301/302 on POST, become a
    ///     bodyless GET; all other redirects preserve the method and body.
    /// </summary>
    public static (ZLinkHttpMethod Method, byte[]? Body) RewriteForRedirect(int status, ZLinkHttpMethod method,
        byte[]? body)
    {
        if (status == 303 || (status is 301 or 302 && method == ZLinkHttpMethod.Post))
            return (ZLinkHttpMethod.Get, null);

        return (method, body);
    }

    public static Uri ResolveLocation(Uri current, string location)
    {
        if (Uri.TryCreate(current, location, out var resolved)
            && resolved.Scheme is "http" or "https")
            return resolved;

        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.ProtocolError,
            $"HTTP redirect location is not supported: {location}");
    }
}
