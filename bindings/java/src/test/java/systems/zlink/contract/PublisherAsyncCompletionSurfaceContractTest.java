/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Method;
import java.util.Arrays;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.PublishOperation;
import systems.zlink.contracts.messaging.PublishSubmitOperation;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.XPubSocket;

class PublisherAsyncCompletionSurfaceContractTest {
    @Test
    void publisherExposesOnlyTheSynchronousSubmitTerminal()
        throws NoSuchMethodException {
        assertEquals(PublishOperation.class,
            PubSocket.class.getMethod("publish", String.class)
                .getReturnType());
        assertEquals(PublishOperation.class,
            XPubSocket.class.getMethod("publish", String.class)
                .getReturnType());
        assertThrows(NoSuchMethodException.class,
            () -> PubSocket.class.getMethod("publishAsync", String.class));
        assertThrows(NoSuchMethodException.class,
            () -> XPubSocket.class.getMethod("publishAsync", String.class));

        Method[] methods = PublishSubmitOperation.class.getMethods();
        assertTrue(Arrays.stream(methods)
            .anyMatch(method -> method.getName().equals("flags")));
        Method[] submits = Arrays.stream(methods)
            .filter(method -> method.getName().equals("submit"))
            .toArray(Method[]::new);
        assertEquals(1, submits.length);
        assertEquals(0, submits[0].getParameterCount());
        assertEquals(void.class, submits[0].getReturnType());
    }
}
