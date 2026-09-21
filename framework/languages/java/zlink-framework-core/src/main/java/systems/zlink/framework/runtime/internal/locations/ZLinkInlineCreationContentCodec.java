package systems.zlink.framework.runtime.internal.locations;

import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Base64;

/**
 * Single owner of the {@code requestContentReference} grammar.
 *
 * <p>21-location-runtime.md#2.4: the reference has the form {@code inline-v1:{base64url}}, where
 * {@code {base64url}} encodes the creation request bytes over the alphabet {@code A-Z a-z 0-9 - _}
 * with no {@code =} padding. No other form is recognized. The same atomic record's {@code
 * requestEncodedSize} and {@code requestSha256} decide the content's integrity, so the reference
 * carries no checksum segment of its own.
 */
public final class ZLinkInlineCreationContentCodec {
    private static final String PREFIX = "inline-v1:";

    private ZLinkInlineCreationContentCodec() {}

    public static String encode(byte[] payload) {
        return PREFIX + Base64.getUrlEncoder().withoutPadding().encodeToString(payload);
    }

    /**
     * Decodes the reference and verifies it against the same record's {@code requestEncodedSize}
     * and {@code requestSha256}. A reference outside the specified form, a length mismatch or a
     * digest mismatch all throw, and the caller must record the creation as failed without running
     * the factory.
     */
    public static byte[] decode(String reference, byte[] expectedSha256, int expectedEncodedSize) {
        if (reference == null || !reference.startsWith(PREFIX)) {
            throw new IllegalArgumentException(
                    "creation content reference is not in the inline-v1 form");
        }
        String encoded = reference.substring(PREFIX.length());
        for (int index = 0; index < encoded.length(); index++) {
            char value = encoded.charAt(index);
            boolean allowed =
                    (value >= 'A' && value <= 'Z')
                            || (value >= 'a' && value <= 'z')
                            || (value >= '0' && value <= '9')
                            || value == '-'
                            || value == '_';
            if (!allowed) {
                throw new IllegalArgumentException(
                        "creation content reference is not in the inline-v1 form");
            }
        }
        if (encoded.length() % 4 == 1) {
            throw new IllegalArgumentException(
                    "creation content reference is not in the inline-v1 form");
        }
        byte[] payload = Base64.getUrlDecoder().decode(encoded);
        if (payload.length != expectedEncodedSize) {
            throw new IllegalArgumentException(
                    "creation content encoded size does not match its reservation");
        }
        if (!MessageDigest.isEqual(sha256(payload), expectedSha256)) {
            throw new IllegalArgumentException(
                    "creation content SHA-256 does not match its reservation");
        }
        return payload;
    }

    private static byte[] sha256(byte[] value) {
        try {
            return MessageDigest.getInstance("SHA-256").digest(value);
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }
}
