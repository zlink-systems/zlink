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
import systems.zlink.contracts.sockets.SendFlags;

class RoutedCompletionSurfaceContractTest {
    @Test
    void routedSendAndRequestExposeCanonicalTerminals()
        throws NoSuchMethodException {
        assertEquals(RoutedSendOperation.class,
            DealerSocket.class.getMethod("send").getReturnType());
        assertEquals(RoutedSendOperation.class,
            RouterSocket.class.getMethod("send",
                systems.zlink.contracts.core.RoutingId.class)
                .getReturnType());

        assertRoutedSendTerminals();
        assertRequestTerminals();
    }

    private static void assertRoutedSendTerminals()
        throws NoSuchMethodException {
        assertNoLegacyTerminals(RoutedSendSubmitOperation.class);
        Method[] submits = submitMethods(RoutedSendSubmitOperation.class);
        assertEquals(1, submits.length);

        Method asyncSubmit = RoutedSendSubmitOperation.class
            .getMethod("submit");
        assertTrue(CompletionStage.class.isAssignableFrom(
            asyncSubmit.getReturnType()));

        Method syncSubmit = RoutedSendSubmitOperation.class
            .getMethod("submit_sync", SendFlags.class);
        assertEquals(void.class, syncSubmit.getReturnType());
    }

    private static void assertRequestTerminals()
        throws NoSuchMethodException {
        assertNoLegacyTerminals(RequestSubmitOperation.class);
        Method[] submits = submitMethods(RequestSubmitOperation.class);
        assertEquals(1, submits.length);

        Method asyncSubmit = RequestSubmitOperation.class.getMethod("submit");
        assertTrue(CompletionStage.class.isAssignableFrom(
            asyncSubmit.getReturnType()));

        Method syncReturn = RequestSubmitOperation.class
            .getMethod("submit_sync", SendFlags.class);
        assertEquals(java.util.List.class, syncReturn.getReturnType());

        Method syncCallback = RequestSubmitOperation.class.getMethod(
            "submit_sync", SendFlags.class, java.util.function.BiConsumer.class);
        assertEquals(void.class, syncCallback.getReturnType());
    }

    private static void assertNoLegacyTerminals(Class<?> type) {
        Method[] methods = type.getMethods();
        assertFalse(Arrays.stream(methods)
            .anyMatch(method -> method.getName().equals("await")));
        assertFalse(Arrays.stream(methods)
            .anyMatch(method -> method.getName().equals("flags")));
    }

    private static Method[] submitMethods(Class<?> type) {
        return Arrays.stream(type.getMethods())
            .filter(method -> method.getName().equals("submit"))
            .toArray(Method[]::new);
    }
}
