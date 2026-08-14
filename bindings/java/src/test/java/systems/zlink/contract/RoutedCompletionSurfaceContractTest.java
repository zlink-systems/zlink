/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Method;
import java.util.Arrays;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.RequestSubmitOperation;
import systems.zlink.contracts.messaging.RoutedSendOperation;
import systems.zlink.contracts.messaging.RoutedSendSubmitOperation;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RouterSocket;

class RoutedCompletionSurfaceContractTest {
    @Test
    void routedSendAndRequestExposeOnlyCompletionStageSubmit()
        throws NoSuchMethodException {
        assertEquals(RoutedSendOperation.class,
            DealerSocket.class.getMethod("send").getReturnType());
        assertEquals(RoutedSendOperation.class,
            RouterSocket.class.getMethod("send",
                systems.zlink.contracts.core.RoutingId.class)
                .getReturnType());

        assertCanonicalTerminal(RoutedSendSubmitOperation.class);
        assertCanonicalTerminal(RequestSubmitOperation.class);
    }

    private static void assertCanonicalTerminal(Class<?> type) {
        Method[] methods = type.getMethods();
        assertFalse(Arrays.stream(methods)
            .anyMatch(method -> method.getName().equals("await")));
        assertFalse(Arrays.stream(methods)
            .anyMatch(method -> method.getName().equals("flags")));
        Method[] submits = Arrays.stream(methods)
            .filter(method -> method.getName().equals("submit"))
            .toArray(Method[]::new);
        assertEquals(1, submits.length);
        assertEquals(0, submits[0].getParameterCount());
        assertTrue(CompletionStage.class.isAssignableFrom(
            submits[0].getReturnType()));
    }
}
