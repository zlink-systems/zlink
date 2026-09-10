/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;

final class NativeMultipartScratch {
    private static final long INITIAL_CAPACITY = 8L;
    final MemorySegment nodeRidPtrOut;
    final MemorySegment seqOut;
    final MemorySegment countOut;
    private MemorySegment parts = MemorySegment.NULL;
    private long capacity;

    NativeMultipartScratch() {
        Arena auto = Arena.ofAuto();
        nodeRidPtrOut = auto.allocate(ValueLayout.ADDRESS);
        seqOut = auto.allocate(ValueLayout.JAVA_LONG);
        countOut = auto.allocate(ValueLayout.JAVA_LONG);
    }

    void reset() {
        countOut.set(ValueLayout.JAVA_LONG, 0, 0L);
        if (capacity == 0L) {
            grow(INITIAL_CAPACITY);
        }
    }

    MemorySegment parts() {
        return parts;
    }

    long capacity() {
        return capacity;
    }

    long requiredCount() {
        return countOut.get(ValueLayout.JAVA_LONG, 0);
    }

    void grow(long newCapacity) {
        if (newCapacity <= capacity) {
            throw new IllegalArgumentException(
                "receive capacity did not grow: " + newCapacity);
        }
        long needed = Math.multiplyExact(
            NativeLayouts.MESSAGE_LAYOUT.byteSize(), newCapacity);
        parts = Arena.ofAuto().allocate(needed,
            NativeLayouts.MESSAGE_LAYOUT.byteAlignment());
        capacity = newCapacity;
    }
}
