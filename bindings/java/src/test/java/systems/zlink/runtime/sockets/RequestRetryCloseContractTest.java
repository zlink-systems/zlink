/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Duration;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.runtime.nativeapi.NativeErrno;

class RequestRetryCloseContractTest {
    @Test
    void closeDuringRequestRetryKeepsStagedPartsUntilSubmitReturns()
        throws Exception {
        CompletionNativeFixture.runProbe(getClass());
    }

    public static void main(String[] args) throws Throwable {
        CompletionNativeFixture core = new CompletionNativeFixture();
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket()) {
            CompletionOwner owner = CompletionNativeFixture.claim(
                (NativeSocketBase) dealer);
            core.attempts.add(new CompletionNativeFixture.Attempt(
                SubmitResult.BACKPRESSURED, NativeErrno.EAGAIN, 41));
            var reply = dealer.request().message(Message.from("staged"))
                .timeout(Duration.ofSeconds(2)).submit().reply()
                .toCompletableFuture();
            core.writable(core.submissions.getLast(), 0, 0);
            core.attempts.add(new CompletionNativeFixture.Attempt(
                SubmitResult.TERMINATED, NativeErrno.ESHUTDOWN, 0, true));
            FutureTask<Integer> drain = new FutureTask<>(owner::drain);
            Thread drainer = Thread.ofPlatform().start(drain);
            try {
                assertTrue(core.admissionEntered.await(
                    TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS));
                dealer.close();
                assertFalse(drain.isDone());
            } finally {
                core.releaseAdmission.countDown();
                drainer.join(TestSupport.DEFAULT_TIMEOUT_MS);
            }
            assertEquals(1, drain.get(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS));
            assertEquals(RequestResult.TERMINATED,
                assertInstanceOf(ZlinkRequestException.class,
                    CompletionNativeFixture.failure(reply)).getResult());
            core.verify(1);
        }
    }
}
