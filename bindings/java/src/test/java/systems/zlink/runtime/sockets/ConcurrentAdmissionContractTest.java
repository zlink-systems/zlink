/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;
import java.time.Duration;
import java.util.concurrent.*;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.*;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.*;

class ConcurrentAdmissionContractTest {
    @Test
    void blockedNativeSendPermitsAnotherSenderAndRequestDrain() throws Exception {
        CompletionNativeFixture.runProbe(getClass());
    }

    public static void main(String[] args) throws Throwable {
        CompletionNativeFixture core = new CompletionNativeFixture();
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket()) {
            CompletionOwner owner = CompletionNativeFixture.claim((NativeSocketBase) dealer);
            core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.OK, 0, 51));
            var request = dealer.request().message(Message.from("request")).timeout(Duration.ofSeconds(2)).submit().toCompletableFuture();
            core.requestResult(core.submissions.getLast(), RequestResult.OK);
            core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.OK, 0, 0, true));
            FutureTask<Void> blocked = new FutureTask<>(() -> {
                dealer.send().message(Message.from("blocked")).submit_sync();
                return null;
            });
            Thread sender = Thread.ofPlatform().start(blocked);
            FutureTask<Void> concurrent = new FutureTask<>(() -> {
                dealer.send().message(Message.from("concurrent")).submit_sync();
                return null;
            });
            FutureTask<Integer> drain = new FutureTask<>(() -> owner.drain(null));
            Thread second = null;
            Thread drainer = null;
            try {
                assertTrue(core.admissionEntered.await(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS));
                core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.OK, 0, 0));
                second = Thread.ofPlatform().start(concurrent);
                drainer = Thread.ofPlatform().start(drain);
                concurrent.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
                assertEquals(1, drain.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS));
                Message.closeAll(request.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS));
                assertFalse(blocked.isDone(), "progress must happen before native admission is released");
            } finally {
                core.releaseAdmission.countDown();
                sender.join(TestSupport.DEFAULT_TIMEOUT_MS);
                if (second != null) second.join(TestSupport.DEFAULT_TIMEOUT_MS);
                if (drainer != null) drainer.join(TestSupport.DEFAULT_TIMEOUT_MS);
            }
            blocked.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
            core.verify(1);
        }
    }
}
