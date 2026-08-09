package systems.zlink.framework.runtime.locations;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;

public final class ZLinkAuthorityKeyCodec {
    private static final char[] HEX = "0123456789ABCDEF".toCharArray();
    private ZLinkAuthorityKeyCodec() {
    }

    public static String spot(String spotId) {
        byte[] identity = systems.zlink.framework.runtime.internal.spots
            .ZLinkSpotIdValidator.requireValid(spotId)
            .getBytes(StandardCharsets.UTF_8);
        return encode("zla1:s:", identity, "Spot");
    }

    public static String actor(String actorId) {
        if (actorId == null || actorId.isBlank()
            || actorId.indexOf('\0') >= 0) {
            throw new IllegalArgumentException("actorId is required");
        }
        byte[] identity = actorId.getBytes(
            StandardCharsets.UTF_8);
        return encode("zla1:a:", identity, "Actor");
    }

    private static String encode(
        String prefix,
        byte[] identity,
        String kind) {
        if (identity.length == 0 || identity.length > 0xff) {
            throw new IllegalArgumentException(
                kind + " authority identity must contain 1..255 bytes");
        }
        StringBuilder encoded = new StringBuilder(
            prefix + identity.length + ":");
        for (byte item : identity) {
            int value = Byte.toUnsignedInt(item);
            if (isUnreserved(value)) {
                encoded.append((char) value);
            } else {
                encoded.append('%');
                encoded.append(HEX[(value >>> 4) & 0xf]);
                encoded.append(HEX[value & 0xf]);
            }
        }
        return encoded.toString();
    }

    static String spotPrefix() {
        return "zla1:s:";
    }

    static String actorPrefix() {
        return "zla1:a:";
    }

    static String actorId(String key) {
        return decode(key, actorPrefix(), "actor");
    }

    static String spotId(String key) {
        return decode(key, spotPrefix(), "spot");
    }

    private static String decode(String key, String prefix, String kind) {
        if (key == null || !key.startsWith(prefix))
            throw new IllegalArgumentException("invalid " + kind + " authority key");
        int lengthEnd = key.indexOf(':', prefix.length());
        if (lengthEnd < 0)
            throw new IllegalArgumentException("invalid " + kind + " authority key");
        int length;
        try {
            length = Integer.parseInt(key.substring(prefix.length(), lengthEnd));
        } catch (NumberFormatException exception) {
            throw new IllegalArgumentException("invalid " + kind + " authority key", exception);
        }
        String encoded = key.substring(lengthEnd + 1);
        ByteArrayOutputStream bytes = new ByteArrayOutputStream(length);
        for (int index = 0; index < encoded.length();) {
            char value = encoded.charAt(index++);
            if (value != '%') {
                if (value > 0x7f)
                    throw new IllegalArgumentException("invalid " + kind + " authority key");
                bytes.write((byte) value);
                continue;
            }
            if (index + 1 >= encoded.length())
                throw new IllegalArgumentException("invalid " + kind + " authority key");
            int high = Character.digit(encoded.charAt(index++), 16);
            int low = Character.digit(encoded.charAt(index++), 16);
            if (high < 0 || low < 0)
                throw new IllegalArgumentException("invalid " + kind + " authority key");
            bytes.write((high << 4) | low);
        }
        if (bytes.size() != length || length == 0 || length > 0xff)
            throw new IllegalArgumentException("invalid " + kind + " authority key");
        return new String(bytes.toByteArray(), StandardCharsets.UTF_8);
    }

    private static boolean isUnreserved(int value) {
        return value >= 'A' && value <= 'Z'
            || value >= 'a' && value <= 'z'
            || value >= '0' && value <= '9'
            || value == '-'
            || value == '.'
            || value == '_'
            || value == '~';
    }
}
