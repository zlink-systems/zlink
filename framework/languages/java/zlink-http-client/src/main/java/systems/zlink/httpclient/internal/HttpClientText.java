/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Base64;
import java.util.UUID;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/** Shared text helpers mirroring the C++ {@code client.cpp} anonymous-namespace utilities. */
public final class HttpClientText {
    private static final long MAX_TIMEOUT_MILLIS = Integer.MAX_VALUE;

    private HttpClientText() {
    }

    public static boolean isBlank(String value) {
        return value == null || value.isBlank();
    }

    public static void requireNonBlank(String value, String message) {
        if (isBlank(value)) {
            throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, message);
        }
    }

    public static void requirePositiveTimeout(Duration value) {
        normalizeTimeout(value);
    }

    /** Returns the contract's millisecond-rounded timeout after validating its finite range. */
    public static Duration normalizeTimeout(Duration value) {
        if (value == null || value.isZero() || value.isNegative()) {
            throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "HTTP client timeout must be greater than zero");
        }
        long millis;
        try {
            millis = value.toMillis();
            if (!value.minusMillis(millis).isZero()) {
                millis = Math.addExact(millis, 1L);
            }
        } catch (ArithmeticException error) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                "HTTP client timeout must fit the finite 1..2147483647 ms range",
                error);
        }
        if (millis < 1L || millis > MAX_TIMEOUT_MILLIS) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                "HTTP client timeout must fit the finite 1..2147483647 ms range");
        }
        return Duration.ofMillis(millis);
    }

    public static String percentEncode(String value) {
        StringBuilder encoded = new StringBuilder();
        for (byte raw : value.getBytes(StandardCharsets.UTF_8)) {
            int by = raw & 0xff;
            boolean unreserved = (by >= 'A' && by <= 'Z')
                || (by >= 'a' && by <= 'z')
                || (by >= '0' && by <= '9')
                || by == '-' || by == '_' || by == '.' || by == '~';
            if (unreserved) {
                encoded.append((char) by);
            } else {
                encoded.append('%').append(String.format("%02X", by));
            }
        }
        return encoded.toString();
    }

    public static String basicAuthorization(String user, String password) {
        String token = Base64.getEncoder()
            .encodeToString((user + ":" + password).getBytes(StandardCharsets.UTF_8));
        return "Basic " + token;
    }

    public static String makeMultipartBoundary() {
        return "zlink-boundary-" + UUID.randomUUID().toString().replace("-", "").substring(0, 16);
    }
}
