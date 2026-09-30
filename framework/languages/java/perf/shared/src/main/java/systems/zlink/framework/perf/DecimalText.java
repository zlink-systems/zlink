package systems.zlink.framework.perf;

// §15.2 U64/I64 decimal strings. Ticks and sequences are Java longs (an unsigned 64-bit value beyond Long.MAX_VALUE
// is never produced by a perf run); a text that is not the canonical form of its value is rejected.
public final class DecimalText {
    private DecimalText() {}

    public static String of(long value) {
        return Long.toString(value);
    }

    public static long u64(String text) {
        try {
            long value = Long.parseUnsignedLong(text);
            if (Long.toUnsignedString(value).equals(text)) {
                return value;
            }
        } catch (NumberFormatException | NullPointerException ignored) {
            // falls through to the error below
        }
        throw new PerfValidationException("SchemaMismatch", "Noncanonical U64 decimal string.");
    }

    public static long i64(String text) {
        try {
            long value = Long.parseLong(text);
            if (Long.toString(value).equals(text)) {
                return value;
            }
        } catch (NumberFormatException | NullPointerException ignored) {
            // falls through to the error below
        }
        throw new PerfValidationException("SchemaMismatch", "Noncanonical I64 decimal string.");
    }
}
