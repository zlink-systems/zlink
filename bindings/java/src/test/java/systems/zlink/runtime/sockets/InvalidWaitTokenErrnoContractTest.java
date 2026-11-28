/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.runtime.nativeapi.NativeErrno;

class InvalidWaitTokenErrnoContractTest {
    @Test
    void invalidWaitTokenErrnoFailsSendAndRequest() throws Exception {
        CompletionNativeFixture.runProbe(getClass());
    }

    public static void main(String[] args) throws Throwable {
        CompletionNativeFixture core = new CompletionNativeFixture();
        byte[] payload = "caller payload".getBytes(StandardCharsets.UTF_8);
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket()) {
            CompletionOwner owner = CompletionNativeFixture.claim((NativeSocketBase) dealer);
            Map<?, ?> pending = (Map<?, ?>) field(owner, "pending");
            for (boolean request : new boolean[] {false, true}) {
                try (Message message = Message.from(payload)) {
                    scriptBackpressure(core, 41, NativeErrno.EINVAL);
                    assertProtocolError(assertThrows(ZlinkSubmitException.class,
                        () -> submit(dealer, message, request)));
                    assertArrayEquals(payload, message.data());
                    assertTrue(pending.isEmpty());
                    assertEquals(0, field(owner, "unpublishedSendCount"));
                    core.writable(core.submissions.getLast(), 0, 0);
                    assertEquals(1, owner.drain());
                }

                try (Message message = Message.from(payload)) {
                    scriptBackpressure(core, 42, NativeErrno.EAGAIN);
                    var admitted = submit(dealer, message, request);
                    assertFalse(admitted.isDone());
                    var state = pending.values().iterator().next();
                    Message staged = (Message) ((List<?>) field(state, "retained")).getFirst();
                    assertArrayEquals(payload, staged.data());
                    scriptBackpressure(core, 43, NativeErrno.EINVAL);
                    core.writable(core.submissions.getLast(), 0, 0);
                    assertEquals(1, owner.drain());
                    assertProtocolError(assertInstanceOf(ZlinkSubmitException.class,
                        CompletionNativeFixture.failure(admitted)));
                    assertTrue(pending.isEmpty());
                    assertTrue(((List<?>) field(state, "retained")).isEmpty());
                    assertEquals(0, staged.size());
                }
            }
            core.verify(4);
        }
    }

    private static CompletableFuture<Void> submit(DealerSocket dealer,
                                                   Message message,
                                                   boolean request) {
        return (request ? dealer.request().message(message).submit().admitted()
            : dealer.send().message(message).submit().admitted()).toCompletableFuture();
    }

    private static void scriptBackpressure(CompletionNativeFixture core, long id,
                                           int errno) {
        core.attempts.add(new CompletionNativeFixture.Attempt(
            SubmitResult.BACKPRESSURED, errno, id));
    }

    private static void assertProtocolError(ZlinkSubmitException error) {
        assertEquals(SubmitResult.INTERNAL_ERROR, error.getResult());
        assertEquals(NativeErrno.EPROTO, error.getNativeErrno());
    }

    private static Object field(Object target, String name) throws ReflectiveOperationException {
        var field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(target);
    }
}
