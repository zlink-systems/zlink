/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;

public final class RecvScratch {
    public static final int TOPIC_CAPACITY = 256;

    public final Arena arena = Arena.ofAuto();
    public final MemorySegment sourceRidOut = arena.allocate(ValueLayout.ADDRESS);
    public final MemorySegment routingIdOut = arena.allocate(
        NativeLayouts.ROUTING_ID_LAYOUT);
    public final MemorySegment subscribedOut = arena.allocate(ValueLayout.JAVA_INT);
    public final MemorySegment hasMoreOut = arena.allocate(ValueLayout.JAVA_INT);

    // Subscribe hot path: keep the native topic-out buffer thread-local so
    // public receive calls avoid allocating scratch storage per message.
    public final MemorySegment topicOut = arena.allocate(TOPIC_CAPACITY);
    public final MemorySegment topicLenOut = arena.allocate(ValueLayout.JAVA_LONG);
}
