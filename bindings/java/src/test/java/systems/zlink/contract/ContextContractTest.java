package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.sockets.AutoHwmProfile;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.core.ContextOptions;
import java.lang.reflect.Method;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class ContextContractTest {
    @Test
    public void rawContextOptionBagIsHiddenAndTypedSurfaceRemains() {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext()) {
            ContextOptions options = ctx.options();
            assertTrue(hasPublicMethod(Context.class, "options"));
            assertEquals(ContextOptions.class, options.getClass());
            assertFalse(hasPublicMethod(Context.class, "setOption"));
            assertFalse(hasPublicMethod(Context.class, "getOption"));
            assertFalse(hasPublicMethod(Context.class, "ioThreads"));
            assertFalse(hasPublicMethod(Context.class, "maxSockets"));
            assertFalse(hasPublicMethod(Context.class, "threadSchedPolicy"));
            assertFalse(hasPublicMethod(Context.class, "messageStructSize"));
            assertFalse(hasPublicMethod(ContextOptions.class, "addThreadAffinityCpu"));
            assertFalse(hasPublicMethod(ContextOptions.class, "removeThreadAffinityCpu"));
            assertTrue(hasPublicMethod(ContextOptions.class, "addThreadAffinityCpu", int.class));
            assertTrue(hasPublicMethod(ContextOptions.class, "removeThreadAffinityCpu", int.class));
            assertDoesNotThrow(() -> options.ioThreads(2));
            assertEquals(2, options.ioThreads());
            assertTrue(options.socketLimit() >= options.maxSockets());
            assertDoesNotThrow(() -> options.blocky(true));
            assertTrue(options.blocky());
            assertDoesNotThrow(() -> options.blocky(false));
            assertFalse(options.blocky());
            assertDoesNotThrow(
                () -> options.autoHwmProfile(AutoHwmProfile.COMPACT));
            assertEquals(AutoHwmProfile.COMPACT, options.autoHwmProfile());
            assertEquals(0L, options.autoHwmMessageUnitBytes());
            long planningUnit = (long) Integer.MAX_VALUE + 1024L;
            assertDoesNotThrow(
                () -> options.autoHwmMessageUnitBytes(planningUnit));
            assertEquals(planningUnit, options.autoHwmMessageUnitBytes());
            assertDoesNotThrow(() -> options.autoHwmMessageUnitBytes(0L));
            assertEquals(0L, options.autoHwmMessageUnitBytes());
            assertDoesNotThrow(() -> options.autoHwmMessageUnitBytes(-1L));
            assertEquals(-1L, options.autoHwmMessageUnitBytes());
            assertTrue(options.messageThreadSize() > 0);
        }
    }

    private static boolean hasPublicMethod(Class<?> type, String name,
                                           Class<?>... parameterTypes) {
        for (Method method : type.getMethods()) {
            if (method.getName().equals(name)
                && java.util.Arrays.equals(method.getParameterTypes(),
                    parameterTypes))
                return true;
        }
        return false;
    }
}
