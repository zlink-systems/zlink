/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import java.net.URI;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.httpclient.ZLinkHttpMethod;

/**
 * Stateless redirect and target-URL rules for the wrapper-owned redirect loop. Mirrors the C++
 * {@code url.cpp} helpers and isolates the redirect contract (allowed statuses, origin format,
 * location resolution, method rewrite) from the request flow.
 */
final class RedirectPolicy {

    /** Result of a redirect method/body rewrite. */
    record Rewrite(ZLinkHttpMethod method, String body) {
    }

    private RedirectPolicy() {
    }

    /** Combines the base URL path prefix with the request target. */
    static String makeTarget(String prefix, String path) {
        if (prefix == null || prefix.isEmpty() || prefix.equals("/")) {
            return path;
        }
        return prefix.endsWith("/") ? prefix.substring(0, prefix.length() - 1) + path : prefix + path;
    }

    static boolean isRedirect(int status) {
        return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    }

    /** Normalised origin (scheme://host:port) with default ports applied, for same-origin checks. */
    static String originOf(URI uri) {
        int port = uri.getPort();
        if (port == -1) {
            port = "https".equals(uri.getScheme()) ? 443 : 80;
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
        if (status == 303 || ((status == 301 || status == 302) && method == ZLinkHttpMethod.POST)) {
            return new Rewrite(ZLinkHttpMethod.GET, null);
        }
        return new Rewrite(method, body);
    }

    static URI resolveLocation(URI current, String location) {
        if (location.startsWith("http://") || location.startsWith("https://")) {
            return URI.create(location);
        }
        if (location.startsWith("//")) {
            // Protocol-relative location: inherit the current scheme.
            return URI.create(current.getScheme() + ":" + location);
        }
        if (location.startsWith("/")) {
            return URI.create(originOf(current) + location);
        }
        throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, "HTTP redirect location is not supported: " + location);
    }
}
