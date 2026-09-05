/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
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

class CompletionOwnerLifecycleTest {
    @Test
    void repeatedBackpressureKeepsOneIdleOwnerUntilContextClose() throws Exception {
        TestSupport.assumeNative();
        Thread worker = null;
        CompletionOwner owner = null;
        Object runtime = null;
        Object wake = null;
        try (Context context = Zlink.createContext()) {
            context.options().autoHwmEnabled(false);
            try (DealerSocket sender = context.createDealerSocket();
                 DealerSocket receiver = context.createDealerSocket();
                 Poller receiverPoller = Zlink.createPoller();
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
                owner = ((NativeSocketBase) sender).runtime().completionOwner();
                runtime = field(owner, "runtime");
                for (int round = 0; round < 8; round++) {
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
                    Thread current = (Thread) field(runtime, "thread");
                    Object currentWake = field(runtime, "wake");
                    assertTrue(current != null && current.isAlive(),
                        "idle completion owner must stay alive");
                    assertTrue(currentWake != null);
                    if (worker == null) {
                        worker = current;
                        wake = currentWake;
                    } else {
                        assertSame(worker, current, "backpressure must reuse its owner");
                        assertSame(wake, currentWake, "backpressure must reuse its control pair");
                    }
                }
            }
            assertTrue(worker.isAlive(), "socket close leaves the Context owner available");
            assertTrue(((java.util.Map<?, ?>) field(runtime, "registrations")).isEmpty(),
                "socket close must release native poller registrations");
        }
        assertFalse(worker.isAlive(), "context close must join the idle owner");
        assertNull(field(runtime, "wake"));
    }

    @Test
    void contextSharesOneWaitOwnerAcrossSocketsAndPublicHandovers() throws Exception {
        TestSupport.assumeNative();
        var senders = new java.util.ArrayList<DealerSocket>();
        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket();
             Poller publicPoller = Zlink.createPoller();
             Received received = new Received()) {
            String endpoint = TestSupport.inprocEndpoint("shared-completion-owner");
            router.bind(endpoint);
            Object runtime = null;
            Object worker = null;
            Object nativePoller = null;
            Object wake = null;
            try {
                // More sockets than virtual-thread carriers catches a native
                // wait per idle virtual thread starving later owners.
                for (int i = 0; i < Runtime.getRuntime().availableProcessors() + 2; i++) {
                    DealerSocket sender = context.createDealerSocket();
                    senders.add(sender);
                    sender.connect(endpoint);
                    var future = sender.request().message(Message.from("request"))
                        .timeout(Duration.ofSeconds(2)).submit().toCompletableFuture();
                    assertTrue(router.recv(received, RecvFlags.NONE));
                    received.reply().message(received.firstPart()).submit();
                    received.close();
                    Message.closeAll(future.get(TestSupport.DEFAULT_TIMEOUT_MS,
                        TimeUnit.MILLISECONDS));
                    Object current = field(((NativeSocketBase) sender).runtime()
                        .completionOwner(), "runtime");
                    if (runtime == null) {
                        runtime = current;
                        worker = field(runtime, "thread");
                        nativePoller = field(runtime, "poller");
                        wake = field(runtime, "wake");
                    }
                    assertSame(runtime, current);
                    assertSame(worker, field(runtime, "thread"));
                    assertSame(nativePoller, field(runtime, "poller"));
                    assertSame(wake, field(runtime, "wake"));
                    publicPoller.add(sender, i, PollEventFlags.POLLCOMPLETION);
                    var publicFuture = sender.request().message(Message.from("public"))
                        .timeout(Duration.ofSeconds(2)).submit().toCompletableFuture();
                    assertTrue(router.recv(received, RecvFlags.NONE));
                    received.reply().message(received.firstPart()).submit();
                    received.close();
                    assertFalse(publicFuture.isDone(), "public wait owns completion progress");
                    assertEquals(1, publicPoller.wait(new PollEvents(1),
                        Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS)));
                    Message.closeAll(publicFuture.get(TestSupport.DEFAULT_TIMEOUT_MS,
                        TimeUnit.MILLISECONDS));
                    assertTrue(publicPoller.remove(sender));
                }
            } finally {
                for (DealerSocket sender : senders)
                    sender.close();
            }
        }
    }

    private static Object field(Object owner, String name) throws Exception {
        var field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(owner);
    }
}
