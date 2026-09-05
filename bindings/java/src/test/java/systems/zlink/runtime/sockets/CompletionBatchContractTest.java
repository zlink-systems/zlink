/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.RecvFlags;

class CompletionBatchContractTest {
    @Test
    void readySocketCanReleaseAnotherSocketsBusyCompletionLane() throws Exception {
        TestSupport.assumeNative();
        CountDownLatch release = new CountDownLatch(1);
        CountDownLatch occupied = new CountDownLatch(1);
        try (Context context = Zlink.createContext();
             DealerSocket first = context.createDealerSocket();
             DealerSocket second = context.createDealerSocket();
             RouterSocket router = context.createRouterSocket();
             Poller poller = Zlink.createPoller();
             Received request = new Received()) {
            String endpoint = TestSupport.inprocEndpoint("completion-batch");
            router.bind(endpoint);
            first.connect(endpoint);
            second.connect(endpoint);
            poller.add(first, 1, PollEventFlags.POLLCOMPLETION);
            poller.add(second, 2, PollEventFlags.POLLCOMPLETION);
            ((NativeSocketBase) first).runtime().dispatchCompletion(() -> {
                occupied.countDown();
                try {
                    release.await();
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                }
            });
            assertTrue(occupied.await(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS));
            var a = first.request().message(Message.from("a"))
                .timeout(Duration.ofSeconds(2)).submit().toCompletableFuture();
            var b = second.request().message(Message.from("b"))
                .timeout(Duration.ofSeconds(2)).submit().toCompletableFuture();
            b.whenComplete((parts, error) -> release.countDown());
            for (int i = 0; i < 2; i++) {
                assertTrue(router.recv(request, RecvFlags.NONE));
                request.reply().message(request.firstPart()).submit();
                request.close();
            }
            FutureTask<Void> wait = new FutureTask<>(() -> {
                PollEvents events = new PollEvents(2);
                while (!a.isDone() || !b.isDone()) {
                    poller.wait(events, Duration.ofSeconds(2));
                }
                return null;
            });
            Thread thread = Thread.ofPlatform().start(wait);
            try {
                wait.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
                Message.closeAll(a.join());
                Message.closeAll(b.join());
            } finally {
                release.countDown();
                thread.join(TestSupport.DEFAULT_TIMEOUT_MS);
            }
        } finally {
            release.countDown();
        }
    }

    @Test
    void repeatedBackpressureSurvivesIdleAndPublicOwnerHandover() throws Exception {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext()) {
            context.options().autoHwmEnabled(false);
            try (DealerSocket sender = context.createDealerSocket();
                 DealerSocket receiver = context.createDealerSocket();
                 Poller receiverPoller = Zlink.createPoller();
                 Poller handover = Zlink.createPoller();
                 Received received = new Received()) {
                sender.options().sendHwm(512L);
                sender.options().recvHwm(512L);
                receiver.options().sendHwm(512L);
                receiver.options().recvHwm(512L);
                String endpoint = TestSupport.inprocEndpoint("completion-wake-reuse");
                receiver.bind(endpoint);
                sender.connect(endpoint);
                receiverPoller.add(receiver, 1, PollEventFlags.POLLIN);
                PollEvents events = new PollEvents(1);
                for (int round = 0; round < 3; round++) {
                    java.util.concurrent.CompletableFuture<Void> pending = null;
                    int submitted = 0;
                    for (int i = 0; i < 1000; i++) {
                        try (Message message = new Message(256)) {
                            message.writeIntLe(0, i);
                            pending = sender.send().message(message).submit()
                                .toCompletableFuture();
                        }
                        submitted++;
                        if (!pending.isDone()) {
                            break;
                        }
                        pending.join();
                    }
                    assertTrue(submitted < 1000, "fill must reach backpressure");
                    for (int i = 0; i < submitted; i++) {
                        assertEquals(1, receiverPoller.wait(events,
                            Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS)));
                        assertTrue(receiver.recv(received, RecvFlags.DONT_WAIT));
                        assertEquals(i, received.firstPart().readIntLe(0));
                        received.close();
                    }
                    pending.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
                    // Transfer waits for the old runtime owner to exit. Removing
                    // this owner leaves the next episode to the ordinary runtime.
                    handover.add(sender, 2, PollEventFlags.POLLCOMPLETION);
                    handover.remove(sender);
                }
            }
        }
    }

    @Test
    void scratchGrowthAndRequestIdsDoNotAffectLaterSubmits() throws Exception {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket();
             RouterSocket router = context.createRouterSocket();
             Poller poller = Zlink.createPoller();
             Received received = new Received()) {
            String endpoint = TestSupport.inprocEndpoint("completion-scratch");
            router.bind(endpoint);
            dealer.connect(endpoint);
            poller.add(dealer, 1, PollEventFlags.POLLCOMPLETION);
            PollEvents events = new PollEvents(1);
            for (int count : new int[] {1, 5, 2, 9, 1}) {
                var request = dealer.request().message(Message.from("part-0"))
                    .timeout(Duration.ofSeconds(2));
                for (int i = 1; i < count; i++) {
                    request.message(Message.from("part-" + i));
                }
                var future = request.submit().toCompletableFuture();
                assertTrue(router.recv(received, RecvFlags.NONE));
                assertEquals(count, received.parts().size());
                var reply = received.reply().message(received.firstPart());
                for (int i = 1; i < received.parts().size(); i++) {
                    reply.message(received.parts().get(i));
                }
                reply.submit();
                received.close();
                while (!future.isDone()) {
                    poller.wait(events, Duration.ofSeconds(2));
                }
                List<Message> parts = future.join();
                try {
                    assertEquals(count, parts.size());
                    for (int i = 0; i < count; i++) {
                        assertEquals("part-" + i, parts.get(i).toUtf8String());
                    }
                } finally {
                    Message.closeAll(parts);
                }
                dealer.send().message(Message.from("send-after-request"))
                    .submit().toCompletableFuture().get(
                        TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
                assertTrue(router.recv(received, RecvFlags.NONE));
                assertTrue(received.replyToken().isEmpty());
                assertEquals("send-after-request", received.firstPart().toUtf8String());
                received.close();
            }
        }
    }
}
