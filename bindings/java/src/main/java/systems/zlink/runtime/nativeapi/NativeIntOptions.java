/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkConfigException;

public final class NativeIntOptions {
    private NativeIntOptions() {
    }

    public static int get(MemorySegment handle, int option,
                          Getter getter) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment nativeValue = arena.allocate(ValueLayout.JAVA_INT);
            MemorySegment len = arena.allocate(ValueLayout.JAVA_LONG);
            len.set(ValueLayout.JAVA_LONG, 0, ValueLayout.JAVA_INT.byteSize());
            int rc = getter.get(handle, option, nativeValue, len);
            if (rc != 0) {
                throw new ZlinkConfigException(ConfigResult.fromValue(rc));
            }
            return nativeValue.get(ValueLayout.JAVA_INT, 0);
        }
    }

    public static void set(MemorySegment handle, int option, int value,
                           Setter setter) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment nativeValue = arena.allocate(ValueLayout.JAVA_INT);
            nativeValue.set(ValueLayout.JAVA_INT, 0, value);
            int rc = setter.set(handle, option, nativeValue,
                ValueLayout.JAVA_INT.byteSize());
            if (rc != 0) {
                throw new ZlinkConfigException(ConfigResult.fromValue(rc));
            }
        }
    }

    @FunctionalInterface
    public interface Getter {
        int get(MemorySegment handle, int option, MemorySegment value,
                MemorySegment len);
    }

    @FunctionalInterface
    public interface Setter {
        int set(MemorySegment handle, int option, MemorySegment value,
                long len);
    }
}
