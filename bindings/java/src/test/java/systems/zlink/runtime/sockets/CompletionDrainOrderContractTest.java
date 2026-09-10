/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;
import java.time.Duration;
import java.util.*;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.*;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.*;
import systems.zlink.runtime.nativeapi.NativeErrno;

class CompletionDrainOrderContractTest {
    @Test
    void writableSendAndRequestResubmitOnlyAfterNoData() throws Exception {
        CompletionNativeFixture.runProbe(getClass());
    }

    public static void main(String[] args) throws Throwable {
        CompletionNativeFixture core = new CompletionNativeFixture();
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket()) {
            CompletionOwner owner = CompletionNativeFixture.claim((NativeSocketBase) dealer);
            for (boolean request : new boolean[] {false, true}) {
                core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.BACKPRESSURED, NativeErrno.EAGAIN, 21));
                var waiter = request
                    ? dealer.request().message(Message.from("retry")).timeout(Duration.ofSeconds(2)).submit().reply().toCompletableFuture()
                    : dealer.send().message(Message.from("retry")).submit().admitted().toCompletableFuture();
                var writable = core.submissions.getLast();
                core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.OK, 0, 22));
                var second = dealer.request().message(Message.from("queued")).timeout(Duration.ofSeconds(2)).submit().reply().toCompletableFuture();
                core.writable(writable, 0);
                core.requestResult(core.submissions.getLast(), RequestResult.TIMED_OUT);
                core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.OK, 0, request ? 23 : 0));
                core.order.clear();
                AtomicReference<Thread> completionThread = new AtomicReference<>();
                second.whenComplete((ignored, failure) ->
                    completionThread.set(Thread.currentThread()));
                Thread drainThread = Thread.currentThread();
                assertEquals(2, owner.drain());
                assertEquals(List.of("recv:21", "recv:22", "recv:NO_DATA", "submit:" + (request ? 23 : 0)), core.order);
                assertTrue(second.isDone(),
                    "public drain must settle terminal completions before returning");
                assertSame(drainThread, completionThread.get(),
                    "public completion owner must settle on its drain thread");
                assertEquals(RequestResult.TIMED_OUT,
                    assertInstanceOf(ZlinkRequestException.class, CompletionNativeFixture.failure(second)).getResult());
                if (request) {
                    assertFalse(waiter.isDone());
                    core.requestResult(core.submissions.getLast(), RequestResult.OK);
                    assertEquals(1, owner.drain());
                }
                waiter.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
            }
            core.verify(5);
        }
    }
}
