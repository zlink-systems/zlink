package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.Socket;
import java.lang.reflect.Method;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class MessageDiagnosticsContractTest {
    @Test
    public void messageDiagnosticsUseCanonicalNames() {
        TestSupport.assumeNative();

        try (Message msg = Message.from("diagnostic")) {
            assertFalse(hasPublicMethod(Message.class, "getProperty",
                String.class));
            assertFalse(hasPublicMethod(Message.class, "property",
                String.class));
            assertEquals(1, msg.refCount());
        }
    }

    private static boolean hasPublicMethod(Class<?> type, String name,
                                           Class<?>... parameterTypes) {
        for (Method method : type.getMethods()) {
            if (method.getName().equals(name)
                && java.util.Arrays.equals(method.getParameterTypes(),
                  parameterTypes)) {
                return true;
            }
        }
        return false;
    }
}
