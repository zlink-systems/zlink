package systems.zlink.framework.runtime.streams;

import systems.zlink.framework.errors.ZLinkConfigurationException;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Base64;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

final class ZLinkAdmissionTickets {
    private static final String HMAC = "HmacSHA256";
    private static final byte[] DOMAIN =
            "zlink-admission-ticket-v1\0".getBytes(StandardCharsets.US_ASCII);
    private static final byte VERSION = 1;
    private static final int SIGNATURE_OFFSET = Byte.BYTES + 2 * Long.BYTES;
    private static final int TICKET_BYTES = SIGNATURE_OFFSET + 32;
    static final int ENCODED_LENGTH = 66;
    private static final long CLOCK_SKEW_MS = 2_000;
    private static final Base64.Decoder DECODER = Base64.getUrlDecoder();
    private static final Base64.Encoder ENCODER = Base64.getUrlEncoder().withoutPadding();

    private final SecretKeySpec currentKey;
    private final SecretKeySpec previousKey;

    ZLinkAdmissionTickets(String key, String previousKey) {
        this.currentKey = readKey(key, "AdmissionQueueKey");
        this.previousKey =
                previousKey == null ? null : readKey(previousKey, "AdmissionQueuePreviousKey");
    }

    /**
     * Connection identity of Authentication Admission §3.5.2 (the MAC input, never in the ticket).
     */
    record TicketConnection(
            byte[] nodeRid, long nodeLifecycleGeneration, String streamName, byte[] sessionRid) {
        TicketConnection {
            nodeRid = nodeRid.clone();
            sessionRid = sessionRid.clone();
        }

        boolean fitsOneByteLengths() {
            return nodeRid.length <= 255
                    && sessionRid.length <= 255
                    && streamName.getBytes(StandardCharsets.UTF_8).length <= 255;
        }

        @Override
        public boolean equals(Object other) {
            return other instanceof TicketConnection that
                    && nodeLifecycleGeneration == that.nodeLifecycleGeneration
                    && streamName.equals(that.streamName)
                    && Arrays.equals(nodeRid, that.nodeRid)
                    && Arrays.equals(sessionRid, that.sessionRid);
        }

        @Override
        public int hashCode() {
            return java.util.Objects.hash(
                    Arrays.hashCode(nodeRid),
                    nodeLifecycleGeneration,
                    streamName,
                    Arrays.hashCode(sessionRid));
        }

        @Override
        public String toString() {
            return "TicketConnection[stream=" + streamName + "]";
        }
    }

    String issue(TicketConnection connection, long expiresAtUnixMs, long epoch) {
        if (!connection.fitsOneByteLengths() || epoch == 0) {
            throw new IllegalArgumentException("Invalid admission ticket identity.");
        }
        byte[] ticket = new byte[TICKET_BYTES];
        ByteBuffer.wrap(ticket).put(VERSION).putLong(expiresAtUnixMs).putLong(epoch);
        byte[] signature = sign(currentKey, connection, ticket);
        System.arraycopy(signature, 0, ticket, SIGNATURE_OFFSET, signature.length);
        return ENCODER.encodeToString(ticket);
    }

    boolean verifies(String encoded, TicketConnection connection, long epoch, long nowUnixMs) {
        if (encoded == null
                || encoded.length() != ENCODED_LENGTH
                || epoch == 0
                || !connection.fitsOneByteLengths()) {
            return false;
        }
        byte[] ticket;
        try {
            ticket = DECODER.decode(encoded);
        } catch (IllegalArgumentException malformed) {
            return false;
        }
        if (ticket.length != TICKET_BYTES
                || ticket[0] != VERSION
                || !ENCODER.encodeToString(ticket).equals(encoded)) {
            return false;
        }
        var fields = ByteBuffer.wrap(ticket, Byte.BYTES, 2 * Long.BYTES);
        long expiresAtUnixMs = fields.getLong();
        if (fields.getLong() != epoch
                || (expiresAtUnixMs <= Long.MAX_VALUE - CLOCK_SKEW_MS
                        && nowUnixMs > expiresAtUnixMs + CLOCK_SKEW_MS)) {
            return false;
        }

        byte[] signature = Arrays.copyOfRange(ticket, SIGNATURE_OFFSET, TICKET_BYTES);
        return MessageDigest.isEqual(sign(currentKey, connection, ticket), signature)
                || (previousKey != null
                        && MessageDigest.isEqual(sign(previousKey, connection, ticket), signature));
    }

    static void validateKey(String key, String option) {
        Arrays.fill(decodeKey(key, option), (byte) 0);
    }

    private static SecretKeySpec readKey(String key, String option) {
        byte[] bytes = decodeKey(key, option);
        try {
            return new SecretKeySpec(bytes, HMAC);
        } finally {
            Arrays.fill(bytes, (byte) 0);
        }
    }

    private static byte[] decodeKey(String key, String option) {
        if (key == null || key.length() < 43 || key.length() > 86) {
            throw invalidKey(option);
        }
        byte[] bytes;
        try {
            bytes = DECODER.decode(key);
        } catch (IllegalArgumentException malformed) {
            throw invalidKey(option);
        }
        if (bytes.length < 32 || bytes.length > 64 || !ENCODER.encodeToString(bytes).equals(key)) {
            Arrays.fill(bytes, (byte) 0);
            throw invalidKey(option);
        }
        return bytes;
    }

    private static ZLinkConfigurationException invalidKey(String option) {
        return new ZLinkConfigurationException(
                option + " must contain an unpadded base64url key encoding 32..64 bytes.");
    }

    private static byte[] sign(SecretKeySpec key, TicketConnection connection, byte[] ticket) {
        try {
            var mac = Mac.getInstance(HMAC);
            mac.init(key);
            mac.update(DOMAIN);
            mac.update((byte) connection.nodeRid().length);
            mac.update(connection.nodeRid());
            mac.update(
                    ByteBuffer.allocate(Long.BYTES)
                            .putLong(connection.nodeLifecycleGeneration())
                            .array());
            byte[] stream = connection.streamName().getBytes(StandardCharsets.UTF_8);
            mac.update((byte) stream.length);
            mac.update(stream);
            mac.update((byte) connection.sessionRid().length);
            mac.update(connection.sessionRid());
            mac.update(ticket, Byte.BYTES, SIGNATURE_OFFSET - Byte.BYTES);
            return mac.doFinal();
        } catch (GeneralSecurityException failure) {
            throw new IllegalStateException("HMAC-SHA256 is unavailable.", failure);
        }
    }
}
