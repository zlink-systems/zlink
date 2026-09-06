/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;
import java.time.Duration;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.*;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.*;
import systems.zlink.runtime.nativeapi.NativeErrno;

class TokenlessBackpressureContractTest {
    @Test
    void tokenlessBackpressurePreservesSubmitResultAndErrno() throws Exception {
        CompletionNativeFixture.runProbe(getClass());
    }

    public static void main(String[] args) throws Throwable {
        CompletionNativeFixture core = new CompletionNativeFixture();
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket()) {
            CompletionOwner owner = CompletionNativeFixture.claim((NativeSocketBase) dealer);
            for (int operation = 0; operation < 5; operation++) {
                int selected = operation;
                core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.BACKPRESSURED, NativeErrno.EAGAIN, 0));
                try (Message message = Message.from("preserved")) {
                    ZlinkSubmitException failure = assertThrows(ZlinkSubmitException.class, () -> {
                        switch (selected) {
                            case 0 -> dealer.send().message(message).submit();
                            case 1 -> dealer.send().message(message).submit_sync();
                            case 2 -> dealer.request().message(message).timeout(Duration.ofSeconds(2)).submit();
                            case 3 -> dealer.request().message(message).timeout(Duration.ofSeconds(2)).submit_sync();
                            case 4 -> owner.submitReply(RoutingId.from(new byte[] {1}), 7, List.of(message));
                            default -> throw new AssertionError(selected);
                        }
                    });
                    assertBackpressure(failure);
                    assertEquals("preserved", message.toUtf8String(), "failed submission preserves managed input");
                }
            }
            for (boolean request : new boolean[] {false, true}) {
                core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.BACKPRESSURED, NativeErrno.EAGAIN, 31));
                var waiter = request
                    ? dealer.request().message(Message.from("retry")).timeout(Duration.ofSeconds(2)).submit().toCompletableFuture()
                    : dealer.send().message(Message.from("retry")).submit().toCompletableFuture();
                core.writable(core.submissions.getLast(), 0);
                core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.BACKPRESSURED, NativeErrno.EAGAIN, 0));
                assertEquals(1, owner.drain(null));
                assertBackpressure(assertInstanceOf(ZlinkSubmitException.class, CompletionNativeFixture.failure(waiter)));
            }
            core.verify(2);
        }
    }

    private static void assertBackpressure(ZlinkSubmitException failure) {
        assertEquals(SubmitResult.BACKPRESSURED, failure.getResult());
        assertEquals(NativeErrno.EAGAIN, failure.getNativeErrno());
    }
}
