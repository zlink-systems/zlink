package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.messaging.Message;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;

public class ByteBufferMessageContractTest {
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
    public void wrapDirectByteBufferIsNotPublic() {
        assertFalse(hasPublicMethod(Message.class, "wrapDirect", ByteBuffer.class));
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
