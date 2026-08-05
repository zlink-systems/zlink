package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.messaging.Message;
import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class MessageCopyWrapContractTest {
    @Test
    public void copyOfByteBufferDoesNotMutateSourceCursor() {
        TestSupport.assumeNative();

        ByteBuffer source = ByteBuffer.wrap("alpha".getBytes(StandardCharsets.UTF_8));
        source.position(1);

        try (Message msg = Message.from(source)) {
            assertEquals(1, source.position());
            assertArrayEquals("lpha".getBytes(StandardCharsets.UTF_8),
                msg.toByteArray());
        }
    }

    @Test
    public void allocateExposesWritableOwnedPayload() {
        TestSupport.assumeNative();

        try (Message msg = Message.allocate(3)) {
            ByteBuffer data = msg.mutableDataBuffer();
            data.put(0, (byte) 0x01);
            data.put(1, (byte) 0x02);
            data.put(2, (byte) 0x03);

            assertArrayEquals(new byte[] {0x01, 0x02, 0x03}, msg.toByteArray());
        }
    }

    @Test
    public void wrapDirectByteBufferIsNotPublic() {
        assertFalse(hasPublicMethod(Message.class, "wrapDirect", ByteBuffer.class));
    }

    @Test
    public void moveIsNotPublicContract() {
        assertFalse(hasPublicMethod(Message.class, "move"));
    }

    @Test
    public void canonicalMessageConveniencesExposeCopyAndEmptyState() {
        try (Message empty = new Message();
             Message source = Message.from("copy-source");
             Message copy = Message.from(source)) {
            assertTrue(empty.isEmpty());
            assertFalse(source.isEmpty());
            assertArrayEquals(source.toByteArray(), copy.toByteArray());
        }
    }

    @Test
    public void fromByteBufDoesNotMutateReaderIndex() {
        TestSupport.assumeNative();

        ByteBuf source = Unpooled.directBuffer();
        source.writeBytes("alpha".getBytes(StandardCharsets.UTF_8));
        source.readerIndex(1);

        try (Message msg = Message.from(source)) {
            assertEquals(1, source.readerIndex());
            assertArrayEquals("lpha".getBytes(StandardCharsets.UTF_8),
                msg.toByteArray());
        } finally {
            source.release();
        }
    }

    @Test
    public void copyToByteBufWritesAtWriterIndex() {
        TestSupport.assumeNative();

        ByteBuf destination = Unpooled.buffer(16);
        destination.writeByte(0x7f);
        try (Message msg = Message.from("gamma")) {
            int written = msg.copyTo(destination);
            assertEquals(5, written);
            assertEquals(6, destination.writerIndex());
            byte[] actual = new byte[5];
            destination.getBytes(1, actual);
            assertArrayEquals("gamma".getBytes(StandardCharsets.UTF_8), actual);
        } finally {
            destination.release();
        }
    }

    private static boolean hasPublicMethod(Class<?> type, String name,
                                           Class<?>... parameterTypes) {
        try {
            Method method = type.getMethod(name, parameterTypes);
            return method != null;
        } catch (NoSuchMethodException ex) {
            return false;
        }
    }
}
