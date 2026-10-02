/* SPDX-License-Identifier: Apache-2.0 */

namespace Zlink.HttpClient.Runtime;

internal static class HttpHeaderLookup
{
    internal const string ContentType = "Content-Type";
    internal const string Authorization = "Authorization";
    internal const string ContentTypeKey = "content-type";
    internal const string AuthorizationKey = "authorization";
    internal const string ContentEncoding = "content-encoding";
    internal const string ContentLength = "content-length";
    internal const string SetCookie = "Set-Cookie";
    internal const string Location = "Location";
    internal const string UserAgent = "User-Agent";
    internal const string Accept = "Accept";
    internal const string AcceptEncoding = "Accept-Encoding";
    internal const string Cookie = "Cookie";
    internal const string ValueSeparator = ", ";

    public static string? Find(IReadOnlyDictionary<string, string> headers, string name)
    {
        return headers.TryGetValue(name, out var value) ? value : null;
    }

    public static IReadOnlyDictionary<string, string> Without(
        IReadOnlyDictionary<string, string> headers,
        params string[] names
    )
    {
        var copy = new Dictionary<string, string>(headers, StringComparer.OrdinalIgnoreCase);
        foreach (var name in names)
            copy.Remove(name);
        return copy;
    }
}
