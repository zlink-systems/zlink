package systems.zlink.netty;

import systems.zlink.contracts.messaging.Message;
import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;

public class NettyAdapterContractTest {

    @Test
    public void fromByteBufDoesNotMutateReaderIndex() {
        ByteBuf source = Unpooled.directBuffer();
        source.writeBytes("alpha".getBytes(StandardCharsets.UTF_8));
        source.readerIndex(1);

        try (Message msg = NettyMessages.from(source)) {
            assertEquals(1, source.readerIndex());
            assertArrayEquals("lpha".getBytes(StandardCharsets.UTF_8),
                msg.toByteArray());
        } finally {
            source.release();
        }
    }

    @Test
    public void copyToByteBufWrites() {
        ByteBuf dest = Unpooled.buffer(16);
        try (Message msg = Message.from("gamma".getBytes(StandardCharsets.UTF_8))) {
            int written = NettyMessages.copyTo(msg, dest);
            assertEquals(5, written);
            byte[] actual = new byte[written];
            dest.getBytes(0, actual);
            assertArrayEquals("gamma".getBytes(StandardCharsets.UTF_8), actual);
        } finally {
            dest.release();
        }
    }

    @Test
    public void nettySurfaceDoesNotExposeBorrowedWrap() {
        assertFalse(hasPublicMethod(NettyMessages.class, "wrap", ByteBuf.class));
        assertFalse(hasPublicMethod(Message.class, "wrapDirect",
            java.nio.ByteBuffer.class));
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
