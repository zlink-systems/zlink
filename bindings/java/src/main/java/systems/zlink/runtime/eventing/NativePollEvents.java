/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.eventing;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;

final class NativePollEvents {
    private static final long POLLER_EVENT_SIZE = 48;
    private static final long EVENT_SOURCE_KIND_OFFSET = 0;
    private static final long EVENT_FD_OFFSET = 16;
    private static final long EVENT_USER_DATA_OFFSET = 32;
    private static final long EVENT_EVENTS_OFFSET = 40;

    private NativePollEvents() {
    }

    static MemorySegment create(int capacity) {
        return Arena.ofAuto().allocate(POLLER_EVENT_SIZE * capacity,
            ValueLayout.ADDRESS.byteAlignment());
    }

    static MemorySegment create(Arena arena, int capacity) {
        return arena.allocate(POLLER_EVENT_SIZE * capacity,
            ValueLayout.ADDRESS.byteAlignment());
    }

    static int sourceKindValue(MemorySegment state, int index) {
        return state.get(ValueLayout.JAVA_INT,
            offset(index, EVENT_SOURCE_KIND_OFFSET));
    }

    static long slot(MemorySegment state, int index) {
        return state.get(ValueLayout.ADDRESS,
            offset(index, EVENT_USER_DATA_OFFSET)).address();
    }

    static int revents(MemorySegment state, int index) {
        return state.get(ValueLayout.JAVA_SHORT,
            offset(index, EVENT_EVENTS_OFFSET));
    }

    static int fd(MemorySegment state, int index) {
        return state.get(ValueLayout.JAVA_INT,
            offset(index, EVENT_FD_OFFSET));
    }

    private static long offset(int index, long fieldOffset) {
        return (long) index * POLLER_EVENT_SIZE + fieldOffset;
    }
}
