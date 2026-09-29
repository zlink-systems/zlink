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
    void writableProjectionAndLaterSubmitKeepTheirCoreResults() throws Exception {
        CompletionNativeFixture.runProbe(getClass());
    }

    public static void main(String[] args) throws Throwable {
        CompletionNativeFixture core = new CompletionNativeFixture();
        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket()) {
            CompletionOwner owner = CompletionNativeFixture.claim((NativeSocketBase) router);
            RoutingId rid = RoutingId.from(new byte[] {4, 1});
            for (boolean request : new boolean[] {false, true}) {
                for (int completionResult : new int[] {801, 802, 803, 999, 0}) {
                    boolean terminal = completionResult != 0;
                    int terminalErrno = completionResult == 801
                        ? NativeErrno.ENOENT
                        : completionResult == 803 ? NativeErrno.EAGAIN
                        : NativeErrno.ENOTCONN;
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
                        core.writable(pending, 0, 0);
                    router.disconnectRid(rid);
                    if (terminal)
                        core.writable(pending, completionResult, terminalErrno);
                    else
                        core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.NOT_CONNECTED, NativeErrno.EHOSTUNREACH, 0));
                    assertEquals(1, owner.drain());
                    Throwable failure = CompletionNativeFixture.failure(waiter);
                    assertSame(failure,
                        CompletionNativeFixture.failure(admitted),
                        "admission and reply must expose the same terminal cause");
                    var submit = assertInstanceOf(ZlinkSubmitException.class, failure);
                    assertEquals(completionResult == 801 ? SubmitResult.NOT_FOUND
                        : completionResult == 803 ? SubmitResult.BACKPRESSURED
                        : completionResult == 999 ? SubmitResult.INTERNAL_ERROR
                        : SubmitResult.NOT_CONNECTED, submit.getResult());
                    assertEquals(completionResult == 999 ? NativeErrno.EPROTO
                        : terminal ? terminalErrno : NativeErrno.EHOSTUNREACH,
                        assertInstanceOf(ZlinkException.class, failure).getNativeErrno());
                }
            }
            core.verify(10);
        }
    }
}
