/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.RequestSubmitOperation;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubmitResult;

class RoutedMultipartAdmissionContractTest {
    private static final int EINVAL = 22;

    @Test
    void multipartRequestUsesOneExactNonblockingRecordAttempt()
        throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket();
             DealerSocket dealer = context.createDealerSocket();
             ExecutorService server = Executors.newSingleThreadExecutor()) {
            String endpoint = TestSupport.inprocEndpoint(
                "routed-multipart-admission");
            dealer.setRoutingId(RoutingId.from("multipart-client"));
            router.bind(endpoint);
            dealer.connect(endpoint);

            var serverResult = server.submit(() -> {
                try (Received request = new Received()) {
                    router.recv(request, RecvFlags.NONE);
                    assertEquals(List.of("first", "second", "third"),
                        request.parts()
                        .stream().map(Message::toUtf8String).toList());
                    request.reply()
                        .message(Message.from("reply-first"))
                        .submit();
                }
            });

            List<Message> reply = dealer.request()
                .message(Message.from("first"))
                .message(Message.from("second"))
                .message(Message.from("third"))
                .timeout(Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS))
                .submit()
                .toCompletableFuture()
                .get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
            try {
                assertEquals(List.of("reply-first"), reply
                    .stream().map(Message::toUtf8String).toList());
            } finally {
                Message.closeAll(reply);
            }
            serverResult.get(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS);
        }
    }

    @Test
    void concurrentMultipartRequestsExposeCoreRejectionAndBindingStagingRetainsOwnership()
        throws Exception {
        TestSupport.assumeNative();

        int threadCount = 8;
        int partsPerAttempt = 256;
        AtomicInteger accepted = new AtomicInteger();
        AtomicInteger rejected = new AtomicInteger();
        CountDownLatch start = new CountDownLatch(1);

        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket();
             DealerSocket dealer = context.createDealerSocket();
             ExecutorService workers = Executors.newFixedThreadPool(threadCount)) {
            String endpoint = TestSupport.inprocEndpoint(
                "concurrent-routed-multipart-admission");
            dealer.setRoutingId(RoutingId.from("multipart-client"));
            router.bind(endpoint);
            dealer.connect(endpoint);

            var warmup = dealer.request()
                .message(Message.from("warmup"))
                .timeout(Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS))
                .submit();
            try (Received request = new Received()) {
                router.recv(request, RecvFlags.NONE);
                request.reply().message(Message.from("ready")).submit();
            }
            List<Message> warmupReply = warmup.toCompletableFuture().get(
                TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
            Message.closeAll(warmupReply);

            List<Future<?>> results = new ArrayList<>(threadCount);
            for (int thread = 0; thread < threadCount; thread++) {
                int threadId = thread;
                results.add(workers.submit(() -> {
                    start.await();
                    submitAttempt(dealer, threadId, 0, partsPerAttempt,
                        accepted, rejected);
                    return null;
                }));
            }
            start.countDown();
            for (Future<?> result : results) {
                result.get(TestSupport.DEFAULT_TIMEOUT_MS,
                    TimeUnit.MILLISECONDS);
            }

            assertTrue(rejected.get() > 0,
                "concurrent multipart submissions must expose Core rejection");
            assertEquals(threadCount, accepted.get() + rejected.get());
        }
    }

    private static void submitAttempt(DealerSocket dealer, int threadId,
                                      int attempt, int partCount,
                                      AtomicInteger accepted,
                                      AtomicInteger rejected) {
        String prefix = threadId + ":" + attempt + ":";
        List<Message> parts = new ArrayList<>(partCount);
        for (int part = 0; part < partCount; part++) {
            parts.add(Message.from(prefix + part));
        }

        try {
            RequestSubmitOperation operation = dealer.request()
                .message(parts.get(0));
            for (int part = 1; part < parts.size(); part++) {
                operation = operation.message(parts.get(part));
            }
            var completion = operation
                .timeout(Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS))
                .submit().toCompletableFuture();
            completion.getNow(null);
            accepted.incrementAndGet();
        } catch (CompletionException failure) {
            Throwable cause = failure.getCause();
            if (!(cause instanceof ZlinkSubmitException submit)
                || submit.getResult() != SubmitResult.INVALID_ARGUMENT
                || submit.getNativeErrno() != EINVAL) {
                throw failure;
            }
            rejected.incrementAndGet();
            for (int part = 0; part < parts.size(); part++) {
                assertEquals(prefix + part, parts.get(part).toUtf8String(),
                    "Java staging must preserve the public part after Core rejects its native copy");
            }
        } finally {
            Message.closeAll(parts);
        }
    }

}
