/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.messaging.RequestSubmitOperation;
import systems.zlink.contracts.messaging.SendSubmission;
import systems.zlink.contracts.messaging.SendSubmitOperation;
import systems.zlink.contracts.sockets.SubmitResult;
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
        assertEquals(SendSubmission.class,
            SendSubmitOperation.class.getMethod("submit").getReturnType());
        assertEquals(void.class,
            SendSubmitOperation.class.getMethod("submit_sync").getReturnType());
        assertEquals(RequestSubmission.class,
            RequestSubmitOperation.class.getMethod("submit").getReturnType());
        assertEquals(java.util.List.class,
            RequestSubmitOperation.class.getMethod("submit_sync")
                .getReturnType());
        assertEquals(SubmitResult.class,
            SendSubmission.class.getMethod("result").getReturnType());
        assertEquals(java.util.concurrent.CompletionStage.class,
            SendSubmission.class.getMethod("admitted").getReturnType());
        assertEquals(SubmitResult.class,
            RequestSubmission.class.getMethod("result").getReturnType());
        assertEquals(java.util.concurrent.CompletionStage.class,
            RequestSubmission.class.getMethod("admitted").getReturnType());
        assertEquals(java.util.concurrent.CompletionStage.class,
            RequestSubmission.class.getMethod("reply").getReturnType());
        assertThrows(NoSuchMethodException.class, () ->
            SendSubmitOperation.class.getMethod("flags",
                systems.zlink.contracts.sockets.SendFlags.class));
    }
}
