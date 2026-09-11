/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import systems.zlink.TestSupport;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

class NativeMessagePayloadViewTest {
    @Test
    void messageDataReturnsDereferenceablePayloadView() {
        TestSupport.assumeNative();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment message = arena.allocate(NativeLayouts.MESSAGE_LAYOUT);
            assertEquals(0, NativeMessage.messageInitSize(message, Long.BYTES));
            try {
                MemorySegment payload = NativeMessage.messageData(message);
                assertTrue(payload.byteSize() >= Long.BYTES,
                    "payload pointer must be expanded by the native descriptor");
                payload.set(ValueLayout.JAVA_LONG_UNALIGNED, 0,
                    0x123456789ABCDEFL);
                assertEquals(0x123456789ABCDEFL,
                    payload.get(ValueLayout.JAVA_LONG_UNALIGNED, 0));
            } finally {
                assertEquals(0, NativeMessage.messageClose(message));
            }
        }
    }
}
