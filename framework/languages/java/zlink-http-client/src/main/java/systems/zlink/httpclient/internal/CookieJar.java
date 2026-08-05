/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import java.util.ArrayList;
import java.util.List;

/**
 * Wrapper-owned cookie jar mirroring the deliberately narrow C++ {@code cookie_jar.cpp} semantics so
 * behaviour matches across languages: host-exact storage (no {@code Domain}), default {@code Path=/},
 * only {@code Path}/{@code Secure}/{@code Max-Age<=0} honoured ({@code Domain}/{@code Expires}
 * ignored), at most 128 cookies per host, and secure cookies sent only on secure requests. The JDK
 * {@code CookieManager} implements full RFC 6265 and would diverge, so it is disabled in favour of
 * this jar.
 */
public final class CookieJar {

    private static final int MAX_COOKIES_PER_HOST = 128;

    private record Cookie(String host, String name, String value, String path, boolean secure) {
    }

    private final List<Cookie> cookies = new ArrayList<>();

    public synchronized void store(String host, String setCookieHeader) {
        String[] segments = setCookieHeader.split(";");
        if (segments.length == 0) {
            return;
        }
        String pair = segments[0].trim();
        int equals = pair.indexOf('=');
        if (equals < 0) {
            return;
        }
        String name = pair.substring(0, equals).trim();
        String value = pair.substring(equals + 1).trim();
        if (name.isEmpty()) {
            return;
        }

        String path = "/";
        boolean secure = false;
        boolean expired = false;
        for (int i = 1; i < segments.length; i++) {
            String attribute = segments[i].trim();
            int attrEquals = attribute.indexOf('=');
            String attrName = (attrEquals < 0 ? attribute : attribute.substring(0, attrEquals)).trim();
            String attrValue = attrEquals < 0 ? "" : attribute.substring(attrEquals + 1).trim();
            if (attrName.equalsIgnoreCase("path") && !attrValue.isEmpty()) {
                path = attrValue;
            } else if (attrName.equalsIgnoreCase("secure")) {
                secure = true;
            } else if (attrName.equalsIgnoreCase("max-age")) {
                try {
                    expired = Long.parseLong(attrValue) <= 0;
                } catch (NumberFormatException ignored) {
                    // ignore malformed max-age
                }
            }
        }

        String finalPath = path;
        cookies.removeIf(c -> c.host().equals(host) && c.name().equals(name) && c.path().equals(finalPath));
        if (expired) {
            return;
        }

        cookies.add(new Cookie(host, name, value, path, secure));
        long countForHost = cookies.stream().filter(c -> c.host().equals(host)).count();
        while (countForHost > MAX_COOKIES_PER_HOST) {
            int oldest = -1;
            for (int i = 0; i < cookies.size(); i++) {
                if (cookies.get(i).host().equals(host)) {
                    oldest = i;
                    break;
                }
            }
            if (oldest < 0) {
                break;
            }
            cookies.remove(oldest);
            countForHost--;
        }
    }

    public synchronized String headerFor(String host, String path, boolean secure) {
        StringBuilder header = new StringBuilder();
        for (Cookie cookie : cookies) {
            if (!cookie.host().equals(host) || (cookie.secure() && !secure) || !pathMatches(path, cookie.path())) {
                continue;
            }
            if (header.length() > 0) {
                header.append("; ");
            }
            header.append(cookie.name()).append('=').append(cookie.value());
        }
        return header.toString();
    }

    private static boolean pathMatches(String requestPath, String cookiePath) {
        if (cookiePath.isEmpty() || cookiePath.equals("/")) {
            return true;
        }
        if (requestPath.equals(cookiePath)) {
            return true;
        }
        if (!requestPath.startsWith(cookiePath)) {
            return false;
        }
        return cookiePath.endsWith("/") || requestPath.charAt(cookiePath.length()) == '/';
    }
}
