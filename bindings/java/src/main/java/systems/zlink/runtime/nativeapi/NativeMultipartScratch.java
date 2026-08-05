/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.List;
import systems.zlink.contracts.messaging.Message;

final class NativeMultipartScratch {
    final MemorySegment nodeRidPtrOut;
    final MemorySegment seqOut;
    final MemorySegment hasMoreOut;
    private MemorySegment parts = MemorySegment.NULL;
    private long partCount;

    NativeMultipartScratch() {
        Arena auto = Arena.ofAuto();
        nodeRidPtrOut = auto.allocate(ValueLayout.ADDRESS);
        seqOut = auto.allocate(ValueLayout.JAVA_LONG);
        hasMoreOut = auto.allocate(ValueLayout.JAVA_INT);
    }

    void reset() {
        parts = MemorySegment.NULL;
        partCount = 0L;
    }

    long partCount() {
        return partCount;
    }

    MemorySegment materializeParts(List<Message> receivedParts) {
        MemorySegment target = allocateParts(receivedParts.size());
        long moved = 0L;
        try {
            for (int i = 0; i < receivedParts.size(); i++) {
                InternalAccess.messageMoveTo(receivedParts.get(i),
                    nthPart(target, i));
                receivedParts.get(i).close();
                moved++;
            }
            return target;
        } catch (RuntimeException ex) {
            if (target.address() != 0 && moved > 0) {
                NativeMessage.multipartClose(target, moved);
            }
            throw ex;
        } finally {
            for (int i = (int) moved; i < receivedParts.size(); i++) {
                try {
                    receivedParts.get(i).close();
                } catch (RuntimeException ignored) {
                }
            }
        }
    }

    MemorySegment allocateParts(long newPartCount) {
        if (newPartCount <= 0) {
            reset();
            return MemorySegment.NULL;
        }
        long needed = NativeLayouts.MESSAGE_LAYOUT.byteSize() * newPartCount;
        if (parts == MemorySegment.NULL || parts.byteSize() < needed) {
            parts = Arena.ofAuto().allocate(needed,
                NativeLayouts.MESSAGE_LAYOUT.byteAlignment());
        }
        partCount = newPartCount;
        return parts;
    }

    private static MemorySegment nthPart(MemorySegment parts, long index) {
        long messageSize = NativeLayouts.MESSAGE_LAYOUT.byteSize();
        return parts.asSlice(index * messageSize, messageSize);
    }
}
