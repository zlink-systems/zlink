/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import java.lang.foreign.Arena;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Function;

public final class NativeListSnapshots {
    private NativeListSnapshots() {}

    public static <T> List<T> read(MemoryLayout entryLayout,
                                   String operation,
                                   NativeListReader reader,
                                   Function<MemorySegment, T> decoder) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment count = arena.allocate(ValueLayout.JAVA_LONG);
            int rc = reader.read(arena, MemorySegment.NULL, count);
            if (rc != 0) {
                throw InternalAccess.zlinkExceptionFromLastError(
                    systems.zlink.contracts.errors.ErrorCategory.CONFIG);
            }
            int available = boundedCount(count.get(ValueLayout.JAVA_LONG, 0),
                Integer.MAX_VALUE);
            if (available == 0) {
                return List.of();
            }

            MemorySegment entries = arena.allocate(entryLayout, available);
            count.set(ValueLayout.JAVA_LONG, 0, available);
            rc = reader.read(arena, entries, count);
            if (rc != 0) {
                throw InternalAccess.zlinkExceptionFromLastError(
                    systems.zlink.contracts.errors.ErrorCategory.CONFIG);
            }
            int actual = boundedCount(count.get(ValueLayout.JAVA_LONG, 0),
                available);
            long stride = entryLayout.byteSize();
            ArrayList<T> out = new ArrayList<>(actual);
            for (int i = 0; i < actual; i++) {
                out.add(decoder.apply(entries.asSlice((long) i * stride,
                    stride)));
            }
            return List.copyOf(out);
        }
    }

    private static int boundedCount(long value, int max) {
        if (value <= 0) {
            return 0;
        }
        if (value > max) {
            return max;
        }
        return (int) value;
    }

    @FunctionalInterface
    public interface NativeListReader {
        int read(Arena arena, MemorySegment entries, MemorySegment count);
    }
}
