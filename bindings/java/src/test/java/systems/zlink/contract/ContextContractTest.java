package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.sockets.AutoHwmProfile;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.core.ContextOptions;
import systems.zlink.contracts.core.CoreHwmBudgetSnapshot;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import java.lang.reflect.Method;
import java.time.Duration;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.Timeout;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class ContextContractTest {
    @ParameterizedTest(name = "completion pump cleanup after {0} shutdown calls")
    @ValueSource(ints = {0, 1, 2})
    @Timeout(10)
    public void closeReleasesCompletionPumpAfterShutdown(int shutdownCalls) {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext()) {
            try (var router = context.createRouterSocket();
                 var dealer = context.createDealerSocket()) {
                Duration timeout = Duration.ofSeconds(2);
                String endpoint = TestSupport.inprocEndpoint("context-close");
                router.options().recvTimeout(timeout);
                router.bind(endpoint);
                dealer.connect(endpoint);
                dealer.request().message(Message.from("request"))
                    .timeout(timeout).submit();
                try (Received received = new Received()) {
                    assertTrue(router.recv(received, RecvFlags.NONE),
                        "request must be received before completion pump teardown");
                }
            }

            for (int i = 0; i < shutdownCalls; i++)
                assertDoesNotThrow(context::shutdown);
            assertDoesNotThrow(context::close);
            assertDoesNotThrow(context::close);
        }
    }

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
                () -> options.coreHwmProfile(AutoHwmProfile.COMPACT));
            assertEquals(AutoHwmProfile.COMPACT, options.coreHwmProfile());

            long memoryLimit = 16L * 1024L * 1024L;
            long coreBudget = 4L * 1024L * 1024L;
            options.coreHwmMemoryLimitBytes(memoryLimit);
            options.coreHwmBudgetBytes(coreBudget);
            assertEquals(memoryLimit, options.coreHwmMemoryLimitBytes());
            assertEquals(coreBudget, options.coreHwmBudgetBytes());
            assertThrows(IllegalArgumentException.class,
                () -> options.coreHwmBudgetBytes(-1L));

            ctx.recalculateAutoHwm();
            CoreHwmBudgetSnapshot before = ctx.coreHwmBudgetSnapshot();
            assertEquals(1, before.abiVersion());
            assertTrue(before.structSize() > 0);
            assertEquals(memoryLimit, before.configuredMemoryLimitBytes());
            assertEquals(coreBudget, before.configuredCoreBudgetBytes());
            assertEquals(coreBudget, before.effectiveCoreBudgetBytes());
            assertTrue(before.budgetPlanningActive());
            assertTrue(before.aggregateHwmValid());
            assertFalse(before.budgetInsufficient());
            assertFalse(before.aggregateOverflow());
            assertEquals(0L, before.applicationAccountedBytes());
            assertEquals(0L, before.outstandingApplicationLeaseCount());
            assertEquals(0L, before.retiredQueueCount());
            assertEquals(0L, before.deferredOriginCreditBytes());
            assertTrue(before.reservedUInt64().stream()
                .allMatch(value -> value == 0L));

            ctx.resetCoreHwmBudgetMetrics();
            CoreHwmBudgetSnapshot after = ctx.coreHwmBudgetSnapshot();
            assertEquals(before.measurementEpoch() + 1L,
                after.measurementEpoch());
            assertEquals(before.budgetGeneration(), after.budgetGeneration());
            assertEquals(before.currentAccountedBytes(),
                after.currentAccountedBytes());
            assertEquals(before.activeDirectionalQueueCount(),
                after.activeDirectionalQueueCount());

            options.coreHwmMemoryLimitBytes(0L);
            options.coreHwmBudgetBytes(0L);
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
