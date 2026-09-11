package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.internal.ContractAccess;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.concurrent.Executors;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class MessageMemoryContractTest {
    @Test
    void primitiveAccessPreservesEndianAtEveryAlignment() {
        TestSupport.assumeNative();
        for (int offset = 0; offset < Long.BYTES; offset++) {
            byte[] expected = new byte[24];
            ByteBuffer bytes = ByteBuffer.wrap(expected);
            try (Message message = new Message(expected.length)) {
                message.fill((byte) 0);
                message.writeLongLe(offset, 0xFEDCBA9876543210L);
                bytes.order(ByteOrder.LITTLE_ENDIAN).putLong(offset, 0xFEDCBA9876543210L);
                assertArrayEquals(expected, message.data());
                assertEquals(bytes.getLong(offset), message.readLongLe(offset));
                assertEquals(bytes.getInt(offset), message.readIntLe(offset));
                assertEquals(bytes.order(ByteOrder.BIG_ENDIAN).getInt(offset), message.readIntBe(offset));

                message.writeIntBe(offset, 0x89ABCDEF);
                bytes.putInt(offset, 0x89ABCDEF);
                assertArrayEquals(expected, message.data());
                message.writeShortBe(offset, (short) 0xABCD);
                bytes.putShort(offset, (short) 0xABCD);
                assertArrayEquals(expected, message.data());
                message.writeIntLe(offset, 0x87654321);
                bytes.order(ByteOrder.LITTLE_ENDIAN).putInt(offset, 0x87654321);
                message.writeByte(offset + 8, (byte) 0xFE);
                expected[offset + 8] = (byte) 0xFE;
                assertEquals((byte) 0xFE, message.readByte(offset + 8));
                assertArrayEquals(expected, message.data());
            }
        }
    }

    @Test
    void arrayRangesEqualityAndFillPreserveBytes() {
        TestSupport.assumeNative();
        for (int length : new int[] {0, 1, 7, 8, 9, 31, 64, 1024, 65536}) {
            byte[] source = new byte[length + 4];
            for (int i = 0; i < source.length; i++)
                source[i] = (byte) (i * 37);
            byte[] expected = Arrays.copyOfRange(source, 1, length + 1);
            try (Message message = Message.from(source, 1, length);
                 Message copy = Message.from(message)) {
                assertArrayEquals(expected, message.data());
                assertTrue(message.contentEquals(expected));
                assertFalse(message.contentEquals(new byte[length + 1]));
                byte[] destination = new byte[length + 4];
                assertEquals(length, message.copyTo(destination, 2));
                assertArrayEquals(expected, Arrays.copyOfRange(destination, 2, length + 2));
                if (length > 0) {
                    for (int mismatch : new int[] {0, length / 2, length - 1}) {
                        expected[mismatch] ^= 1;
                        assertFalse(message.contentEquals(expected));
                        expected[mismatch] ^= 1;
                    }
                    message.fill((byte) 0xAA, 1, length - 1);
                    Arrays.fill(expected, 1, length, (byte) 0xAA);
                    assertArrayEquals(expected, message.data());
                    assertArrayEquals(Arrays.copyOfRange(source, 1, length + 1), copy.data());
                    assertEquals(length, message.copyFrom(source, 2, 0, length));
                    assertArrayEquals(Arrays.copyOfRange(source, 2, length + 2), message.data());
                    assertEquals(length - 1, message.copyTo(destination, 1, 3, length - 1));
                    assertArrayEquals(Arrays.copyOfRange(source, 3, length + 2),
                        Arrays.copyOfRange(destination, 3, length + 2));
                }
            }
        }
    }

    @Test
    void overlappingMessageCopyHasMemmoveSemantics() {
        TestSupport.assumeNative();
        for (int sourceOffset : new int[] {0, 1, 5}) {
            for (int destinationOffset : new int[] {0, 1, 5}) {
                byte[] bytes = new byte[32];
                for (int i = 0; i < bytes.length; i++)
                    bytes[i] = (byte) i;
                try (Message message = Message.from(bytes)) {
                    System.arraycopy(bytes, sourceOffset, bytes, destinationOffset, 24);
                    assertEquals(24, message.copyFrom(message, sourceOffset, destinationOffset, 24));
                    assertArrayEquals(bytes, message.data());
                }
            }
        }
    }

    @Test
    void invalidRangesAndEmptyReadsKeepTheirErrors() {
        TestSupport.assumeNative();
        Message message = new Message(8);
        try {
            assertThrows(IndexOutOfBoundsException.class, () -> message.readLongLe(1));
            assertThrows(IndexOutOfBoundsException.class, () -> message.readIntBe(-1));
            assertThrows(IndexOutOfBoundsException.class, () -> message.writeIntLe(Integer.MAX_VALUE, 1));
            assertThrows(IndexOutOfBoundsException.class, () -> message.fill((byte) 0, 1, 8));
            assertThrows(IndexOutOfBoundsException.class, () -> message.copyFrom(new byte[8], 0, 1, 8));
            assertThrows(IndexOutOfBoundsException.class, () -> message.copyTo(new byte[8], 1));
            assertThrows(NullPointerException.class, () -> message.contentEquals(null));
            assertEquals(0, message.copyFrom(new byte[0], 0, 8, 0));
        } finally {
            message.close();
        }
        try (Message empty = new Message(0)) {
            assertEquals(0, empty.size());
            assertArrayEquals(new byte[0], empty.data());
            assertTrue(empty.contentEquals(new byte[0]));
            assertThrows(IndexOutOfBoundsException.class, () -> empty.readByte(0));
        }
    }

    @Test
    void moveAndSharedCopyKeepPayloadOwnership() {
        TestSupport.assumeNative();
        // Cover both an inline native payload and a separately allocated payload.
        for (int size : new int[] {9, 128}) {
            try (Message source = Message.from(new byte[size]);
                 Message target = new Message()) {
                source.writeLongLe(1, 0x123456789ABCDEFL);
                try (Message shared = ContractAccess.messageSharedCopyOf(source)) {
                    assertEquals(size, ContractAccess.messageMoveInto(source, target, true));
                    assertEquals(0, source.size());
                    assertTrue(target.more());
                    assertEquals(0x123456789ABCDEFL, target.readLongLe(1));
                    assertEquals(0x123456789ABCDEFL, shared.readLongLe(1));
                }
                assertEquals(0x123456789ABCDEFL, target.readLongLe(1));
            }
        }
    }

    @Test
    void reusedTargetReplacesPayloadAcrossInlineHeapAndEmptyFrames() {
        TestSupport.assumeNative();
        try (Message target = Message.from(new byte[128])) {
            for (int size : new int[] {9, 128, 0, 9, 65536, 0, 128}) {
                // Populate both views before replacing the target's frame.
                target.dataBuffer();
                if (!target.empty())
                    target.readByte(0);
                byte[] expected = new byte[size];
                Arrays.fill(expected, (byte) size);
                try (Message source = Message.from(expected)) {
                    assertEquals(size, ContractAccess.messageMoveInto(source, target, false));
                    assertEquals(0, source.size());
                    assertEquals(0, source.dataBuffer().remaining());
                    assertArrayEquals(expected, target.data());
                    assertTrue(target.contentEquals(expected));
                    assertEquals(size, target.dataBuffer().remaining());
                    if (size > 0) {
                        target.writeLongLe(1, 0x123456789ABCDEFL);
                        assertEquals(0x123456789ABCDEFL, target.readLongLe(1));
                    }
                }
                ContractAccess.messageResetReusable(target);
                assertEquals(0, target.size());
                assertEquals(0, target.dataBuffer().remaining());
            }
        }
    }

    @Test
    void slotsCanBeUsedAndReleasedAcrossThreadsIncludingPoolOverflow() throws Exception {
        TestSupport.assumeNative();
        // Exceed both bounded caches so this also exercises explicit arena close.
        Message[] messages = new Message[4200];
        for (int i = 0; i < messages.length; i++) {
            messages[i] = new Message(9);
            messages[i].writeLongLe(1, i);
        }
        try (var executor = Executors.newSingleThreadExecutor()) {
            executor.submit(() -> {
                for (int i = 0; i < messages.length; i++) {
                    try (Message message = messages[i]) {
                        assertEquals(i, message.readLongLe(1));
                    }
                }
            }).get();
            executor.submit(() -> {
                try (Message reused = Message.from("reused")) {
                    assertEquals("reused", reused.toUtf8String());
                }
            }).get();
        }
        try (Message reused = Message.from("owner")) {
            assertEquals("owner", reused.toUtf8String());
        }
    }
}
