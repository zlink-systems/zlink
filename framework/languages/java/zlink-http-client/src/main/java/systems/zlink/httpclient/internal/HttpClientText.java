/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Base64;
import java.util.UUID;

/** Shared text helpers mirroring the C++ {@code client.cpp} anonymous-namespace utilities. */
public final class HttpClientText {
    public enum Header {
        CONTENT_TYPE("content-type"),
        AUTHORIZATION("authorization"),
        ACCEPT("accept"),
        USER_AGENT("user-agent"),
        ACCEPT_ENCODING("accept-encoding"),
        COOKIE("cookie"),
        SET_COOKIE("set-cookie"),
        LOCATION("location"),
        CONTENT_ENCODING("content-encoding"),
        CONTENT_LENGTH("content-length");

        private final String wire;

        Header(String wire) {
            this.wire = wire;
        }

        public String wire() {
            return wire;
        }
    }

    private enum Scheme {
        HTTP("http", 80),
        HTTPS("https", 443);

        private final String wire;
        private final String prefix;
        private final int defaultPort;

        Scheme(String wire, int defaultPort) {
            this.wire = wire;
            this.prefix = wire + "://";
            this.defaultPort = defaultPort;
        }

        static Scheme decode(String wire) {
            return HTTPS.wire.equals(wire) ? HTTPS : HTTP;
        }
    }

    public static final String JSON_CONTENT_TYPE = "application/json";
    public static final String BASIC_AUTHORIZATION_PREFIX = "Basic ";
    private static final String MULTIPART_BOUNDARY_PREFIX = "zlink-boundary-";
    private static final int MULTIPART_BOUNDARY_RANDOM_LENGTH = 16;
    private static final long MAX_TIMEOUT_MILLIS = Integer.MAX_VALUE;

    private HttpClientText() {}

    public static boolean hasHttpPrefix(String value) {
        return value.startsWith(Scheme.HTTP.prefix);
    }

    public static boolean hasSupportedSchemePrefix(String value) {
        return hasHttpPrefix(value) || value.startsWith(Scheme.HTTPS.prefix);
    }

    public static boolean isSecureScheme(String scheme) {
        return Scheme.decode(scheme) == Scheme.HTTPS;
    }

    static int defaultPort(String scheme) {
        return isSecureScheme(scheme) ? Scheme.HTTPS.defaultPort : Scheme.HTTP.defaultPort;
    }

    public static boolean isBlank(String value) {
        return value == null || value.isBlank();
    }

    public static void requireNonBlank(String value, String message) {
        if (isBlank(value)) {
            throw HttpClientErrors.protocol(message);
        }
    }

    public static void requirePositiveTimeout(Duration value) {
        normalizeTimeout(value);
    }

    /** Returns the contract's millisecond-rounded timeout after validating its finite range. */
    public static Duration normalizeTimeout(Duration value) {
        if (value == null || value.isZero() || value.isNegative()) {
            throw HttpClientErrors.protocol("HTTP client timeout must be greater than zero");
        }
        long millis;
        try {
            millis = value.toMillis();
            if (!value.minusMillis(millis).isZero()) {
                millis = Math.addExact(millis, 1L);
            }
        } catch (ArithmeticException error) {
            throw HttpClientErrors.protocol(
                    "HTTP client timeout must fit the finite 1.."
                            + MAX_TIMEOUT_MILLIS
                            + " ms range",
                    error);
        }
        if (millis < 1L || millis > MAX_TIMEOUT_MILLIS) {
            throw HttpClientErrors.protocol(
                    "HTTP client timeout must fit the finite 1.."
                            + MAX_TIMEOUT_MILLIS
                            + " ms range");
        }
        return Duration.ofMillis(millis);
    }

    public static String percentEncode(String value) {
        StringBuilder encoded = new StringBuilder();
        for (byte raw : value.getBytes(StandardCharsets.UTF_8)) {
            int by = raw & 0xff;
            boolean unreserved =
                    (by >= 'A' && by <= 'Z')
                            || (by >= 'a' && by <= 'z')
                            || (by >= '0' && by <= '9')
                            || by == '-'
                            || by == '_'
                            || by == '.'
                            || by == '~';
            if (unreserved) {
                encoded.append((char) by);
            } else {
                encoded.append('%').append(String.format("%02X", by));
            }
        }
        return encoded.toString();
    }

    public static String basicAuthorization(String user, String password) {
        String token =
                Base64.getEncoder()
                        .encodeToString((user + ":" + password).getBytes(StandardCharsets.UTF_8));
        return BASIC_AUTHORIZATION_PREFIX + token;
    }

    public static String makeMultipartBoundary() {
        return MULTIPART_BOUNDARY_PREFIX
                + UUID.randomUUID()
                        .toString()
                        .replace("-", "")
                        .substring(0, MULTIPART_BOUNDARY_RANDOM_LENGTH);
    }
}
