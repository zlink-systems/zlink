/* SPDX-License-Identifier: Apache-2.0 */

using System.Net;

namespace Zlink.HttpClient.Runtime;

/// <summary>
///     Stateless redirect and target-URL rules for the wrapper-owned redirect loop. Keeping these in one
///     place mirrors the C++ <c>url.cpp</c> helpers and isolates the redirect contract (allowed statuses,
///     origin comparison, location resolution, method rewrite) from the request flow.
/// </summary>
internal static class HttpRedirectPolicy
{
    private const string RootUrlPath = "/";

    /// <summary>Combines the base URL path prefix with the request target.</summary>
    public static string MakeTarget(string prefix, string path)
    {
        if (prefix.Length == 0 || prefix == RootUrlPath)
            return path;

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
        return (HttpStatusCode)status
            is HttpStatusCode.MovedPermanently
                or HttpStatusCode.Found
                or HttpStatusCode.SeeOther
                or HttpStatusCode.TemporaryRedirect
                or HttpStatusCode.PermanentRedirect;
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
    public static (ZLinkHttpMethod Method, byte[]? Body) RewriteForRedirect(
        int status,
        ZLinkHttpMethod method,
        byte[]? body
    )
    {
        if (
            (HttpStatusCode)status == HttpStatusCode.SeeOther
            || (
                (HttpStatusCode)status is HttpStatusCode.MovedPermanently or HttpStatusCode.Found
                && method == ZLinkHttpMethod.Post
            )
        )
            return (ZLinkHttpMethod.Get, null);

        return (method, body);
    }

    public static Uri ResolveLocation(Uri current, string location)
    {
        if (
            Uri.TryCreate(current, location, out var resolved)
            && (resolved.Scheme == Uri.UriSchemeHttp || resolved.Scheme == Uri.UriSchemeHttps)
        )
            return resolved;

        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.ProtocolError,
            $"HTTP redirect location is not supported: {location}"
        );
    }
}
