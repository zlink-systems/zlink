package systems.zlink.framework.locationprovider;

import java.nio.charset.StandardCharsets;

final class ZLinkStoreCounterDecimal {
    private ZLinkStoreCounterDecimal() {}

    static long decode(byte[] bytes) {
        if (bytes == null) return 0;
        try {
            String decimal = new String(bytes, StandardCharsets.US_ASCII);
            long value = Long.parseLong(decimal);
            if (value >= 0 && Long.toString(value).equals(decimal)) return value;
        } catch (NumberFormatException invalid) {
            throw new ArithmeticException("Invalid Location Store counter");
        }
        throw new ArithmeticException("Invalid Location Store counter");
    }
}
