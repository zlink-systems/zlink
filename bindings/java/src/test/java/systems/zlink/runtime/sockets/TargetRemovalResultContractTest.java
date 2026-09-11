/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.*;
import systems.zlink.contracts.errors.*;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.messaging.SendSubmission;
import systems.zlink.contracts.sockets.*;
import systems.zlink.runtime.nativeapi.NativeErrno;

class TargetRemovalResultContractTest {
    @Test
    void removalTerminalAndLaterSubmitKeepTheirCoreResults() throws Exception {
        CompletionNativeFixture.runProbe(getClass());
    }

    public static void main(String[] args) throws Throwable {
        CompletionNativeFixture core = new CompletionNativeFixture();
        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket()) {
            CompletionOwner owner = CompletionNativeFixture.claim((NativeSocketBase) router);
            RoutingId rid = RoutingId.from(new byte[] {4, 1});
            for (boolean request : new boolean[] {false, true}) {
                for (boolean terminal : new boolean[] {true, false}) {
                    core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.BACKPRESSURED, NativeErrno.EAGAIN, 41));
                    CompletableFuture<Void> admitted;
                    CompletableFuture<?> waiter;
                    if (request) {
                        RequestSubmission submission = router.request(rid)
                            .message(Message.from("pending"))
                            .timeout(Duration.ofSeconds(2)).submit();
                        assertEquals(SubmitResult.BACKPRESSURED,
                            submission.result());
                        admitted = submission.admitted().toCompletableFuture();
                        waiter = submission.reply().toCompletableFuture();
                    } else {
                        SendSubmission submission = router.send(rid)
                            .message(Message.from("pending")).submit();
                        assertEquals(SubmitResult.BACKPRESSURED,
                            submission.result());
                        admitted = submission.admitted().toCompletableFuture();
                        waiter = admitted;
                    }
                    var pending = core.submissions.getLast();
                    if (!terminal)
                        core.writable(pending, 0);
                    router.disconnectRid(rid);
                    if (terminal)
                        core.writable(pending, NativeErrno.ENOENT);
                    else
                        core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.NOT_CONNECTED, NativeErrno.EHOSTUNREACH, 0));
                    assertEquals(1, owner.drain());
                    Throwable failure = CompletionNativeFixture.failure(waiter);
                    assertSame(failure,
                        CompletionNativeFixture.failure(admitted),
                        "admission and reply must expose the same terminal cause");
                    if (terminal && request) {
                        assertEquals(RequestResult.NOT_FOUND, assertInstanceOf(ZlinkRequestException.class, failure).getResult());
                    } else {
                        var submit = assertInstanceOf(ZlinkSubmitException.class, failure);
                        assertEquals(terminal ? SubmitResult.NOT_FOUND : SubmitResult.NOT_CONNECTED, submit.getResult());
                    }
                    assertEquals(terminal ? NativeErrno.ENOENT : NativeErrno.EHOSTUNREACH,
                        assertInstanceOf(ZlinkException.class, failure).getNativeErrno());
                }
            }
            core.verify(4);
        }
    }
}
