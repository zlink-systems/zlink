/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import systems.zlink.httpclient.ZLinkHttpMethod;

import java.net.HttpURLConnection;
import java.net.URI;

/**
 * Stateless redirect and target-URL rules for the wrapper-owned redirect loop. Mirrors the C++
 * {@code url.cpp} helpers and isolates the redirect contract (allowed statuses, origin format,
 * location resolution, method rewrite) from the request flow.
 */
final class RedirectPolicy {

    private static final int HTTP_TEMPORARY_REDIRECT = 307;
    private static final int HTTP_PERMANENT_REDIRECT = 308;

    /** Result of a redirect method/body rewrite. */
    record Rewrite(ZLinkHttpMethod method, String body) {}

    private RedirectPolicy() {}

    /** Combines the base URL path prefix with the request target. */
    static String makeTarget(String prefix, String path) {
        if (prefix == null || prefix.isEmpty() || prefix.equals("/")) {
            return path;
        }
        return prefix.endsWith("/")
                ? prefix.substring(0, prefix.length() - 1) + path
                : prefix + path;
    }

    static boolean isRedirect(int status) {
        return status == HttpURLConnection.HTTP_MOVED_PERM
                || status == HttpURLConnection.HTTP_MOVED_TEMP
                || status == HttpURLConnection.HTTP_SEE_OTHER
                || status == HTTP_TEMPORARY_REDIRECT
                || status == HTTP_PERMANENT_REDIRECT;
    }

    /**
     * Normalised origin (scheme://host:port) with default ports applied, for same-origin checks.
     */
    static String originOf(URI uri) {
        int port = uri.getPort();
        if (port == -1) {
            port = HttpClientText.defaultPort(uri.getScheme());
        }
        return uri.getScheme() + "://" + uri.getHost() + ":" + port;
    }

    /** The request path without query, used for cookie path matching. */
    static String pathOf(URI uri) {
        String path = uri.getRawPath();
        return path == null || path.isEmpty() ? "/" : path;
    }

    /**
     * Applies the method/body rewrite for a followed redirect: 303, and 301/302 on POST, become a
     * bodyless GET; all other redirects preserve the method and body.
     */
    static Rewrite rewriteMethodAndBody(int status, ZLinkHttpMethod method, String body) {
        if (status == HttpURLConnection.HTTP_SEE_OTHER
                || ((status == HttpURLConnection.HTTP_MOVED_PERM
                                || status == HttpURLConnection.HTTP_MOVED_TEMP)
                        && method == ZLinkHttpMethod.POST)) {
            return new Rewrite(ZLinkHttpMethod.GET, null);
        }
        return new Rewrite(method, body);
    }

    static URI resolveLocation(URI current, String location) {
        try {
            if (HttpClientText.hasSupportedSchemePrefix(location)) {
                return URI.create(location);
            }
            if (location.startsWith("//")) {
                // Protocol-relative location: inherit the current scheme.
                return URI.create(current.getScheme() + ":" + location);
            }
            if (location.startsWith("/")) {
                return URI.create(originOf(current) + location);
            }
        } catch (IllegalArgumentException cause) {
            throw HttpClientErrors.protocol(
                    "HTTP redirect location is not supported: " + location, cause);
        }
        throw HttpClientErrors.protocol("HTTP redirect location is not supported: " + location);
    }
}
