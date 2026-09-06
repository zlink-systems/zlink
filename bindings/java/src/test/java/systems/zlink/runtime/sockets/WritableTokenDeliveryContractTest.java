/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.*;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.*;
import systems.zlink.runtime.nativeapi.NativeErrno;

class WritableTokenDeliveryContractTest {
    @Test
    void writableReachesOnlyTheWaiterIdentifiedByItsToken() throws Exception {
        CompletionNativeFixture.runProbe(getClass());
    }

    public static void main(String[] args) throws Throwable {
        CompletionNativeFixture core = new CompletionNativeFixture();
        // Deliberately perturb Core's echo to detect a second RID decision in
        // the binding. Public behavior with conforming Core is unchanged.
        core.omitRidEcho = true;
        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket()) {
            CompletionOwner owner = CompletionNativeFixture.claim((NativeSocketBase) router);
            RoutingId rid = RoutingId.from(new byte[] {1, 2, 3});
            core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.BACKPRESSURED, NativeErrno.EAGAIN, 11));
            core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.BACKPRESSURED, NativeErrno.EAGAIN, 12));
            var first = router.send(rid).message(Message.from("first")).submit().toCompletableFuture();
            var second = router.send(rid).message(Message.from("second")).submit().toCompletableFuture();
            core.writable(core.submissions.get(1), 0);
            core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.OK, 0, 0));
            assertEquals(1, owner.drain(null));
            second.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
            assertFalse(first.isDone(), "another token's waiter must remain pending");
            core.writable(core.submissions.get(0), 0);
            core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.OK, 0, 0));
            assertEquals(1, owner.drain(null));
            first.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
            core.verify(2);
        }
    }
}
