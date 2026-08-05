package systems.zlink.framework.runtime.locations;

public final class ZLinkAuthorityKeyCodec {
    private static final char[] HEX = "0123456789ABCDEF".toCharArray();
    private ZLinkAuthorityKeyCodec() {
    }

    public static String spot(String spotId) {
        byte[] identity = systems.zlink.framework.runtime.internal.spots
            .ZLinkSpotIdValidator.requireValid(spotId)
            .getBytes(java.nio.charset.StandardCharsets.UTF_8);
        return encode("zla1:s:", identity, "Spot");
    }

    public static String actor(String actorId) {
        if (actorId == null || actorId.isBlank()
            || actorId.indexOf('\0') >= 0) {
            throw new IllegalArgumentException("actorId is required");
        }
        byte[] identity = actorId.getBytes(
            java.nio.charset.StandardCharsets.UTF_8);
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
