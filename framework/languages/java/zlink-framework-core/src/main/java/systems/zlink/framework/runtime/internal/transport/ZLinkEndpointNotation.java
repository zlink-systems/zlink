package systems.zlink.framework.runtime.internal.transport;

import java.util.Locale;

/**
 * Deterministic, lossless endpoint-string normalization.
 *
 * <p>Implements {@code doc/plan/endpoint-notation-policy.ko.md} &sect;2.2-2.4
 * for Java: lowercase scheme, lowercase host, unify IPv6 literals to bracket
 * notation (zone id preserved verbatim, case included), decimal ports with
 * leading zeros stripped, trailing slash removed from the path, and the
 * whole string trimmed of surrounding whitespace.
 *
 * <p>This mirrors {@code framework/languages/cpp/framework/src/runtime/
 * transport/endpoint_notation.hpp} (commit dfcb2177c9) field-for-field: same
 * authority-scheme allowlist ({@code tcp}/{@code tls}/{@code ws}/{@code wss}),
 * same bracket-aware host/port split, same "refuse to guess" fallback for
 * inputs that cannot be re-shaped without assuming their meaning.
 *
 * <p><b>Why a hand-rolled parser instead of {@code java.net.URI}.</b> The
 * host language has a URI parser, and the policy says to prefer it -- but
 * {@code java.net.URI} is unsuitable for this job. Experimentally (see the
 * task record for this change):
 * <ul>
 *   <li>{@code new URI("tcp://my_service:80")} does not throw, but
 *   {@code getHost()} returns {@code null} because underscores are illegal
 *   in an RFC 3986 reg-name -- and Docker/Kubernetes service hostnames
 *   commonly contain underscores. The structured accessors silently stop
 *   working for a legitimate endpoint.</li>
 *   <li>The 7-arg {@code URI(scheme, userInfo, host, port, path, query,
 *   fragment)} constructor -- which {@code ZLinkChannelSocketRegistry} used
 *   to rebuild the advertised endpoint with a substituted host -- actively
 *   throws {@code URISyntaxException} for that same underscore host,
 *   turning a normal advertise-host configuration into a startup crash.</li>
 *   <li>{@code new URI("tcp://2001:db8::1:80")} (an unbracketed IPv6
 *   literal) does not throw either; it just returns {@code getHost() ==
 *   null} with no signal that the input was ambiguous, which is exactly the
 *   {@code lastIndexOf(':')}-style guessing &sect;2.4 forbids.</li>
 * </ul>
 * {@code java.net.URI} is, however, well-behaved for well-formed input (it
 * preserves IPv6 zone ids without throwing, unlike the WHATWG {@code URL}
 * the Node implementation had to abandon) -- it is simply too strict about
 * the reg-name grammar for endpoints this framework must still accept. A
 * small bracket- and colon-count-aware parser, matching the C++ approach,
 * avoids both failure modes while keeping the "refuse to guess" contract.
 *
 * <p>This is intentionally NOT a validator: {@link #normalize} never
 * throws. Input it cannot confidently re-shape (no scheme, a non-authority
 * scheme such as {@code ipc}, or an authority it cannot parse without
 * guessing) is returned with only whitespace trimmed and -- when a scheme
 * is present -- the scheme lowercased. Callers that need to reject
 * malformed endpoints keep doing so where they already do.
 *
 * <p>Per &sect;2.3 this belongs at write time: call it once when an
 * endpoint is constructed (advertised endpoint assembly) or accepted from
 * outside the process (application-supplied bind/remote endpoints, peer
 * descriptor endpoints read off the wire, Location Store rows). Callers
 * elsewhere keep comparing normalized strings with plain {@code equals}.
 */
public final class ZLinkEndpointNotation {

    private ZLinkEndpointNotation() {
    }

    /**
     * Normalizes an endpoint string per the policy above. Total and
     * non-throwing: unparseable input is trimmed (and scheme-lowercased,
     * if a scheme is present) and returned as-is rather than corrupted.
     * Returns {@code null} for {@code null} input.
     */
    public static String normalize(String raw) {
        if (raw == null) {
            return null;
        }
        String trimmed = raw.strip();
        if (trimmed.isEmpty()) {
            return trimmed;
        }
        int schemeEnd = trimmed.indexOf("://");
        if (schemeEnd < 0) {
            return trimmed;
        }
        String schemeLower = trimmed.substring(0, schemeEnd).toLowerCase(Locale.ROOT);
        String rest = trimmed.substring(schemeEnd + 3);

        if (!isAuthorityScheme(schemeLower)) {
            // Non-authority scheme (e.g. "ipc" -- a filesystem path follows,
            // not a host) or an unknown scheme: only the scheme casing is a
            // safe, lossless normalization. The remainder is opaque.
            return schemeLower + "://" + rest;
        }

        Authority authority = Authority.parse(rest);
        if (authority == null) {
            // Malformed or ambiguous authority: refuse to guess (§2.4),
            // hand back the trimmed original untouched.
            return trimmed;
        }

        StringBuilder result = new StringBuilder(trimmed.length() + 2);
        result.append(schemeLower).append("://").append(authority.userInfo);
        if (authority.isIpv6) {
            result.append('[').append(normalizeIpv6Host(authority.host)).append(']');
        } else {
            result.append(authority.host.toLowerCase(Locale.ROOT));
        }
        if (!authority.port.isEmpty()) {
            result.append(':').append(stripLeadingZeroDigits(authority.port));
        }
        result.append(stripTrailingSlashes(authority.path)).append(authority.tail);
        return result.toString();
    }

    /**
     * Wraps {@code host} in "[...]" when it is an unbracketed IPv6-shaped
     * literal (contains ':'), otherwise returns it unchanged. Zone ids
     * travel inside the brackets untouched. Shared by every endpoint
     * construction seam so IPv6 formatting is identical everywhere a TCP
     * endpoint string is assembled. Does not lowercase -- pass the result
     * through {@link #normalize} for that.
     */
    public static String bracketIpv6Host(String host) {
        if (host == null || host.isEmpty() || host.charAt(0) == '['
            || host.indexOf(':') < 0) {
            return host;
        }
        return "[" + host + "]";
    }

    /**
     * Replaces the host of a {@code scheme://[userInfo@]host[:port][/path]}
     * endpoint with {@code newHost}, using the same IPv6-safe (bracket- and
     * colon-count-aware, never {@code lastIndexOf(':')}) parsing as
     * {@link #normalize}. Scheme, host, and path are formatted per policy
     * as a side effect. Refuses to guess: an endpoint or host it cannot
     * parse/format unambiguously is returned as {@code endpoint} unchanged.
     */
    public static String withHost(String endpoint, String newHost) {
        if (endpoint == null || newHost == null || newHost.isBlank()) {
            return endpoint;
        }
        String trimmed = endpoint.strip();
        int schemeEnd = trimmed.indexOf("://");
        if (schemeEnd < 0) {
            return endpoint;
        }
        String schemeLower = trimmed.substring(0, schemeEnd).toLowerCase(Locale.ROOT);
        String rest = trimmed.substring(schemeEnd + 3);
        if (!isAuthorityScheme(schemeLower)) {
            return endpoint;
        }
        Authority authority = Authority.parse(rest);
        if (authority == null) {
            return endpoint;
        }
        StringBuilder result = new StringBuilder();
        result.append(schemeLower).append("://").append(authority.userInfo);
        result.append(formatHost(newHost));
        if (!authority.port.isEmpty()) {
            result.append(':').append(stripLeadingZeroDigits(authority.port));
        }
        result.append(stripTrailingSlashes(authority.path)).append(authority.tail);
        return result.toString();
    }

    private static String formatHost(String host) {
        String trimmed = host.strip();
        boolean bracketed = trimmed.length() >= 2
            && trimmed.charAt(0) == '[' && trimmed.charAt(trimmed.length() - 1) == ']';
        String inner = bracketed ? trimmed.substring(1, trimmed.length() - 1) : trimmed;
        boolean ipv6 = bracketed || inner.indexOf(':') >= 0;
        return ipv6 ? "[" + normalizeIpv6Host(inner) + "]" : inner.toLowerCase(Locale.ROOT);
    }

    private static boolean isAuthorityScheme(String schemeLower) {
        return schemeLower.equals("tcp") || schemeLower.equals("tls")
            || schemeLower.equals("ws") || schemeLower.equals("wss");
    }

    private static boolean allAsciiDigits(String value) {
        if (value.isEmpty()) {
            return false;
        }
        for (int index = 0; index < value.length(); index++) {
            char character = value.charAt(index);
            if (character < '0' || character > '9') {
                return false;
            }
        }
        return true;
    }

    private static String stripLeadingZeroDigits(String port) {
        int start = 0;
        while (start + 1 < port.length() && port.charAt(start) == '0') {
            start++;
        }
        return port.substring(start);
    }

    private static String stripTrailingSlashes(String path) {
        int end = path.length();
        while (end > 0 && path.charAt(end - 1) == '/') {
            end--;
        }
        return path.substring(0, end);
    }

    private static String normalizeIpv6Host(String hostRaw) {
        int zone = hostRaw.indexOf('%');
        String ipPart = zone < 0 ? hostRaw : hostRaw.substring(0, zone);
        String lowered = ipPart.toLowerCase(Locale.ROOT);
        return zone < 0 ? lowered : lowered + hostRaw.substring(zone);
    }

    /** Parsed {@code [userInfo@]host[:port]} authority plus path/query/fragment tail. */
    private static final class Authority {
        final String userInfo;
        final String host;
        final boolean isIpv6;
        final String port;
        final String path;
        final String tail;

        private Authority(
            String userInfo,
            String host,
            boolean isIpv6,
            String port,
            String path,
            String tail) {
            this.userInfo = userInfo;
            this.host = host;
            this.isIpv6 = isIpv6;
            this.port = port;
            this.path = path;
            this.tail = tail;
        }

        /** Returns {@code null} when the authority cannot be parsed without guessing. */
        static Authority parse(String rest) {
            int authorityEnd = indexOfAny(rest, "/?#");
            String authority = authorityEnd < 0 ? rest : rest.substring(0, authorityEnd);
            String remainder = authorityEnd < 0 ? "" : rest.substring(authorityEnd);

            String userInfo = "";
            String hostPort = authority;
            int at = authority.lastIndexOf('@');
            if (at >= 0) {
                userInfo = authority.substring(0, at + 1);
                hostPort = authority.substring(at + 1);
            }

            String hostRaw;
            String port = "";
            boolean isIpv6 = false;

            if (!hostPort.isEmpty() && hostPort.charAt(0) == '[') {
                int close = hostPort.indexOf(']');
                if (close < 0) {
                    // Malformed bracket: refuse to guess.
                    return null;
                }
                hostRaw = hostPort.substring(1, close);
                isIpv6 = true;
                String after = hostPort.substring(close + 1);
                if (!after.isEmpty()) {
                    if (after.charAt(0) != ':' || !allAsciiDigits(after.substring(1))) {
                        return null;
                    }
                    port = after.substring(1);
                }
            } else {
                int colonCount = 0;
                for (int index = 0; index < hostPort.length(); index++) {
                    if (hostPort.charAt(index) == ':') {
                        colonCount++;
                    }
                }
                if (colonCount == 0) {
                    hostRaw = hostPort;
                } else if (colonCount == 1) {
                    int colon = hostPort.indexOf(':');
                    String candidatePort = hostPort.substring(colon + 1);
                    if (allAsciiDigits(candidatePort)) {
                        hostRaw = hostPort.substring(0, colon);
                        port = candidatePort;
                    } else {
                        // A single colon but no valid port after it -- not a
                        // shape this function can safely re-normalize.
                        // Refuse to guess (§2.4).
                        return null;
                    }
                } else {
                    // 2+ colons with no brackets: an unbracketed IPv6
                    // literal. lastIndexOf(':')-style port splitting is
                    // exactly what §2.4 forbids, so no port is extracted
                    // here -- only bracketing.
                    hostRaw = hostPort;
                    isIpv6 = true;
                }
            }

            int qpos = indexOfAny(remainder, "?#");
            String path = qpos < 0 ? remainder : remainder.substring(0, qpos);
            String tail = qpos < 0 ? "" : remainder.substring(qpos);

            return new Authority(userInfo, hostRaw, isIpv6, port, path, tail);
        }

        private static int indexOfAny(String value, String chars) {
            for (int index = 0; index < value.length(); index++) {
                if (chars.indexOf(value.charAt(index)) >= 0) {
                    return index;
                }
            }
            return -1;
        }
    }
}
