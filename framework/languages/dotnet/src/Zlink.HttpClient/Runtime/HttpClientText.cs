/* SPDX-License-Identifier: Apache-2.0 */

using System.Text;

namespace Zlink.HttpClient.Runtime;

/// <summary>Shared text helpers mirroring the C++ <c>client.cpp</c> anonymous-namespace utilities.</summary>
internal static class HttpClientText
{
    public static bool IsBlank(string value)
    {
        return string.IsNullOrEmpty(value) || value.All(ch => ch is ' ' or '\t' or '\r' or '\n');
    }

    public static void RequireNonBlank(string value, string message)
    {
        if (IsBlank(value)) throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.ProtocolError, message);
    }

    public static void RequirePositiveTimeout(TimeSpan value)
    {
        if (value <= TimeSpan.Zero)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "HTTP client timeout must be greater than zero");
    }

    public static string PercentEncode(string value)
    {
        var encoded = new StringBuilder(value.Length);
        foreach (var by in Encoding.UTF8.GetBytes(value))
        {
            var unreserved = by is >= (byte)'A' and <= (byte)'Z'
                or >= (byte)'a' and <= (byte)'z'
                or >= (byte)'0' and <= (byte)'9'
                or (byte)'-' or (byte)'_' or (byte)'.' or (byte)'~';
            if (unreserved)
                encoded.Append((char)by);
            else
                encoded.Append('%').Append(by.ToString("X2"));
        }

        return encoded.ToString();
    }

    public static string BasicAuthorization(string user, string password)
    {
        return "Basic " + Convert.ToBase64String(Encoding.UTF8.GetBytes($"{user}:{password}"));
    }

    public static string MakeMultipartBoundary()
    {
        return "zlink-boundary-" + Guid.NewGuid().ToString("N");
    }
}