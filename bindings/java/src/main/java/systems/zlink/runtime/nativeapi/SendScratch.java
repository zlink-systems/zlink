/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;

public final class SendScratch {
    public final Arena arena = Arena.ofConfined();
    public final MemorySegment nativeMsg = arena.allocate(NativeLayouts.MESSAGE_LAYOUT);
    public final MemorySegment nativeRoutingId = arena.allocate(
        NativeLayouts.ROUTING_ID_LAYOUT);
}
