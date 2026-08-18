namespace Zlink.Framework.Runtime.Configuration;

/// <summary>
///     Deterministic, lossless string normalization for endpoint notation
///     (doc/plan/endpoint-notation-policy.ko.md). For schemes that carry a
///     real network authority (tcp/tls/ws/wss), normalizes scheme and host
///     casing, IPv6 bracket notation (preserving zone id), decimal port
///     without leading zeros, and trims one trailing path slash.
///     userInfo, path, query, and fragment are preserved verbatim. For every
///     other scheme (e.g. <c>ipc://</c>, whose remainder is a filesystem
///     path rather than a network authority), only the scheme is lowercased
///     -- every remaining byte is kept exactly as written, since host/port/
///     slash rules do not apply to a path. Surrounding whitespace is always
///     trimmed. Applied once at the point an endpoint is constructed or
///     accepted from outside the process; downstream comparisons stay plain
///     Ordinal string equality.
/// </summary>
/// <remarks>
///     No DNS resolution ever happens here: <c>localhost</c> and
///     <c>127.0.0.1</c> normalize to themselves and remain distinct
///     endpoints. Input that cannot be parsed as an absolute URI (missing
///     scheme, malformed authority, etc.) is returned trimmed and otherwise
///     unchanged rather than throwing, since callers such as
///     <see cref="ZLinkNetworkEndpointResolver.Bind"/> historically accept
///     arbitrary explicit endpoint strings.
/// </remarks>
internal static class ZLinkEndpointNotation
{
    // Schemes with a real dialable network authority (host[:port]). Every
    // other scheme's remainder is opaque (most commonly a filesystem path,
    // e.g. ipc:///var/run/x.sock, or a process-local identity, e.g.
    // inproc://name) and must not have host/port/slash rules applied to it.
    private static readonly string[] AuthoritySchemes = ["tcp", "tls", "ws", "wss"];

    internal static string Normalize(string? endpoint)
    {
        if (endpoint is null)
            return string.Empty;
        var trimmed = endpoint.Trim();
        if (trimmed.Length == 0)
            return trimmed;

        var scheme = TryExtractLowercaseScheme(trimmed, out var schemeLength);
        if (scheme is null)
            return trimmed;

        if (!IsAuthorityScheme(scheme))
            // Opaque scheme: lowercase the scheme token only. Everything
            // from the colon onward (including path casing, slash count,
            // and any query-like suffix) is preserved byte-for-byte.
            return scheme + trimmed[schemeLength..];

        if (!Uri.TryCreate(trimmed, UriKind.Absolute, out var uri))
            return trimmed;

        var userInfo = uri.GetComponents(UriComponents.UserInfo, UriFormat.UriEscaped);
        var host = NormalizeHost(trimmed, uri);
        var path = uri.GetComponents(UriComponents.Path, UriFormat.UriEscaped);
        if (path.EndsWith('/'))
            path = path[..^1];
        var query = uri.GetComponents(UriComponents.Query, UriFormat.UriEscaped);
        var fragment = uri.GetComponents(UriComponents.Fragment, UriFormat.UriEscaped);

        var builder = new System.Text.StringBuilder();
        builder.Append(uri.Scheme).Append("://");
        if (userInfo.Length > 0)
            builder.Append(userInfo).Append('@');
        builder.Append(host);
        if (uri.Port != -1)
            builder.Append(':').Append(uri.Port);
        if (path.Length > 0)
            builder.Append('/').Append(path);
        if (query.Length > 0)
            builder.Append('?').Append(query);
        if (fragment.Length > 0)
            builder.Append('#').Append(fragment);
        return builder.ToString();
    }

    /// <summary>
    ///     Whether <paramref name="scheme"/> carries a real network
    ///     authority (host[:port]) that host-substitution and the
    ///     tcp-connector's scheme dispatch may act on. Shared with
    ///     <see cref="ZLinkNetworkEndpointResolver"/> so the two stay in
    ///     lock-step with the schemes actually normalized here.
    /// </summary>
    internal static bool IsAuthorityScheme(string scheme) =>
        Array.IndexOf(AuthoritySchemes, scheme) >= 0;

    private static string? TryExtractLowercaseScheme(string value, out int schemeLength)
    {
        schemeLength = 0;
        var colon = value.IndexOf(':');
        if (colon <= 0)
            return null;
        var candidate = value[..colon];
        if (!IsValidSchemeToken(candidate))
            return null;
        schemeLength = colon;
        return candidate.ToLowerInvariant();
    }

    private static bool IsValidSchemeToken(string candidate)
    {
        if (!IsAsciiLetter(candidate[0]))
            return false;
        foreach (var character in candidate)
            if (!IsAsciiLetter(character)
                && !IsAsciiDigit(character)
                && character != '+'
                && character != '-'
                && character != '.')
                return false;
        return true;
    }

    private static bool IsAsciiLetter(char character) =>
        character is >= 'a' and <= 'z' or >= 'A' and <= 'Z';

    private static bool IsAsciiDigit(char character) =>
        character is >= '0' and <= '9';

    private static string NormalizeHost(string original, Uri uri)
    {
        // Dns and IPv4 hosts are already lowercased by Uri itself.
        if (uri.HostNameType != UriHostNameType.IPv6)
            return uri.Host;

        // Uri.Host drops the IPv6 zone id entirely, so it must be recovered
        // from the original text and reattached in canonical %25 form.
        var bracketed = uri.Host;
        var innerAddress = bracketed.Length >= 2
            ? bracketed[1..^1]
            : bracketed;
        var zoneId = ExtractZoneId(original);
        return zoneId is null ? $"[{innerAddress}]" : $"[{innerAddress}%25{zoneId}]";
    }

    private static string? ExtractZoneId(string original)
    {
        var open = original.IndexOf('[');
        if (open < 0)
            return null;
        var close = original.IndexOf(']', open + 1);
        if (close < 0)
            return null;
        var bracketContent = original[(open + 1)..close];
        var percent = bracketContent.IndexOf('%');
        if (percent < 0)
            return null;
        var raw = bracketContent[(percent + 1)..];
        if (raw.StartsWith("25", StringComparison.Ordinal))
            raw = raw[2..];
        return raw.Length == 0 ? null : raw;
    }
}
