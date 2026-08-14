/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Method;
import java.util.Arrays;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.AsyncSendOperation;
import systems.zlink.contracts.messaging.AsyncSendSubmitOperation;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.XPubSocket;

class PublisherAsyncCompletionSurfaceContractTest {
    @Test
    void publisherAsyncUsesTheCanonicalCompletionStageTerminal()
        throws NoSuchMethodException {
        assertEquals(AsyncSendOperation.class,
            PubSocket.class.getMethod("publishAsync", String.class)
                .getReturnType());
        assertEquals(AsyncSendOperation.class,
            XPubSocket.class.getMethod("publishAsync", String.class)
                .getReturnType());

        Method[] methods = AsyncSendSubmitOperation.class.getMethods();
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
