package systems.zlink.runtime.nativeapi;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;

public final class NativeHelpers {
    private NativeHelpers() {}

    public static MemorySegment toCString(Arena arena, String value) {
        return arena.allocateFrom(value, StandardCharsets.UTF_8);
    }

    public static String fromCString(MemorySegment segment, int maxLen) {
        if (segment == null || segment.address() == 0)
            return "";
        int len = cStringLength(segment, maxLen);
        if (len == 0)
            return "";
        byte[] bytes = new byte[len];
        MemorySegment.copy(segment, 0, MemorySegment.ofArray(bytes), 0, len);
        return new String(bytes, 0, len, StandardCharsets.UTF_8);
    }

    public static int cStringLength(MemorySegment segment, int maxLen) {
        if (segment == null || segment.address() == 0 || maxLen <= 0)
            return 0;
        int len = 0;
        while (len < maxLen) {
            if (segment.get(java.lang.foreign.ValueLayout.JAVA_BYTE, len) == 0)
                break;
            len++;
        }
        return len;
    }
}
