/* SPDX-License-Identifier: Apache-2.0 */

namespace Zlink.HttpClient.Runtime;

/// <summary>
///     Version identity for outgoing requests. The single source of truth is the
///     <c>&lt;Version&gt;</c> property in <c>Zlink.HttpClient.csproj</c>; the User-Agent
///     product token is derived from it as <c>zlink-http-client/&lt;major.minor&gt;</c>.
/// </summary>
internal static class HttpClientVersion
{
    public static readonly string UserAgent =
        "zlink-http-client/" + (typeof(HttpClientVersion).Assembly.GetName().Version is { } version
            ? $"{version.Major}.{version.Minor}"
            : "0.0");
}
