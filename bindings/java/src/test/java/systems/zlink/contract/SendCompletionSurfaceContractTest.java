/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.SendSubmitOperation;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.StreamSocket;

final class SendCompletionSurfaceContractTest {
    @Test
    void everySendBuilderUsesOneCompletionContract() throws Exception {
        assertEquals(systems.zlink.contracts.messaging.SendOperation.class,
            PairSocket.class.getMethod("send").getReturnType());
        assertEquals(systems.zlink.contracts.messaging.SendOperation.class,
            DealerSocket.class.getMethod("send").getReturnType());
        assertEquals(systems.zlink.contracts.messaging.SendOperation.class,
            RouterSocket.class.getMethod("send",
                systems.zlink.contracts.core.RoutingId.class).getReturnType());
        assertEquals(systems.zlink.contracts.messaging.SendOperation.class,
            StreamSocket.class.getMethod("send",
                systems.zlink.contracts.core.RoutingId.class).getReturnType());
        assertEquals(CompletionStage.class,
            SendSubmitOperation.class.getMethod("submit").getReturnType());
        assertEquals(void.class,
            SendSubmitOperation.class.getMethod("submit_sync").getReturnType());
        assertThrows(NoSuchMethodException.class, () ->
            SendSubmitOperation.class.getMethod("flags",
                systems.zlink.contracts.sockets.SendFlags.class));
    }
}
