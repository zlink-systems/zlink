package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.channels.ZLinkRequestCallerAllocationTest;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceSendHeaderAllocationTest;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;

/** Measures actual public binding calls; peer receive/reply allocations are outside the filter. */
public final class ZLinkBindingRequestAllocationTest {
    private static final int SAMPLES = 1_000;
    private static final Duration WAIT = Duration.ofSeconds(2);
    private static final RoutingId TARGET = RoutingId.from("binding-allocation-target");

    @Test
    void countsOneThousandNativeRequestsAndCancelledLateReplies() throws Exception {
        ZLinkServiceSendHeaderAllocationTest.runInAllocationJvm(getClass());
    }

    public static void main(String[] args) throws Exception {
        System.out.println(
                "binding-request-allocation samples=1000 transport=inproc backend=actual-binding "
                        + "scope=callerSubmit+callerComplete peerAllocations=excluded "
                        + "bindingJniCalls=0(binding-uses-FFM) nativeFfmCrossings=unmeasured "
                        + "timeoutBaseline=unmeasured(no-deterministic-native-timer) "
                        + "lateReplyStorageRelease=unmeasured-internal");
        try (Fixture fixture = new Fixture()) {
            for (int index = 0; index < 100; index++) {
                fixture.roundTrip(false);
                fixture.roundTrip(true);
            }
            measure(fixture, false);
            measure(fixture, true);
            // A fresh successful operation proves cancellation did not consume the reply capacity.
            fixture.roundTrip(false);
        }
    }

    private static void measure(Fixture fixture, boolean cancellation) throws Exception {
        fixture.requests = fixture.callbacks = fixture.cancelled = fixture.drained = 0;
        var count =
                ZLinkRequestCallerAllocationTest.allocations(
                        Fixture.class,
                        List.of("callerSubmit", "callerComplete"),
                        () -> {
                            for (int index = 0; index < SAMPLES; index++)
                                fixture.roundTrip(cancellation);
                        });
        assertEquals(SAMPLES, fixture.requests);
        assertEquals(SAMPLES, fixture.callbacks);
        assertEquals(SAMPLES, fixture.drained);
        assertEquals(cancellation ? SAMPLES : 0, fixture.cancelled);
        assertTrue(count.objects() >= SAMPLES, "real binding allocation events are present");
        System.out.printf(
                "binding terminal=%s requests=%d callbacks=%d drained=%d allocations=%s%n",
                cancellation ? "CANCELLATION" : "REPLY",
                fixture.requests,
                fixture.callbacks,
                fixture.drained,
                count);
    }

    private static final class Fixture implements AutoCloseable {
        final Context context = Zlink.createContext();
        final RouterSocket target = context.createRouterSocket();
        final RouterSocket caller = context.createRouterSocket();
        final Poller receive = Zlink.createPoller();
        final Poller completion = Zlink.createPoller();
        final PollEvents receivedEvents = new PollEvents(1);
        final PollEvents completionEvents = new PollEvents(1);
        final Message header = Message.from("header");
        final Message payload = Message.from(new byte[1_024]);
        final Message reply = Message.from("reply");
        int requests;
        int callbacks;
        int cancelled;
        int drained;

        Fixture() {
            target.setRoutingId(TARGET);
            caller.setRoutingId(RoutingId.from("binding-allocation-source"));
            target.options().linger(Duration.ZERO);
            caller.options().linger(Duration.ZERO);
            String endpoint = "inproc://binding-request-allocation-" + System.nanoTime();
            target.bind(endpoint);
            caller.connect(endpoint);
            receive.add(target, 1, PollEventFlags.POLLIN);
            completion.add(caller, 1, PollEventFlags.POLLCOMPLETION);
        }

        CompletableFuture<List<Message>> callerSubmit(boolean cancellation) {
            var submitted =
                    caller.request(TARGET)
                            .message(Message.from(header))
                            .message(Message.from(payload))
                            .timeout(WAIT)
                            .submit();
            if (submitted.result() != SubmitResult.OK)
                throw new AssertionError(
                        "expected inproc request admission: " + submitted.result());
            requests++;
            var future = submitted.reply().toCompletableFuture();
            future.whenComplete((parts, failure) -> callbacks++);
            if (cancellation) {
                if (!future.cancel(false)) throw new AssertionError("request was already terminal");
                cancelled++;
            }
            return future;
        }

        void roundTrip(boolean cancellation) {
            Zlink.has(ZLinkServiceSendHeaderAllocationTest.NATIVE_CALLER_BOUNDARY);
            var future = callerSubmit(cancellation);
            Zlink.has(ZLinkServiceSendHeaderAllocationTest.NATIVE_CALLER_BOUNDARY);
            if (receive.wait(receivedEvents, WAIT) != 1)
                throw new AssertionError("native target did not receive its request");
            try (Received request = new Received()) {
                if (!target.recv(request, RecvFlags.DONT_WAIT))
                    throw new AssertionError("native request readiness was not consumable");
                if (request.parts().size() != 2)
                    throw new AssertionError("request multipart changed");
                request.reply().message(Message.from(reply)).submit();
            }
            Zlink.has(ZLinkServiceSendHeaderAllocationTest.NATIVE_CALLER_BOUNDARY);
            callerComplete(future, cancellation);
            Zlink.has(ZLinkServiceSendHeaderAllocationTest.NATIVE_CALLER_BOUNDARY);
        }

        void callerComplete(CompletableFuture<List<Message>> future, boolean cancellation) {
            if (completion.wait(completionEvents, WAIT) != 1
                    || !completionEvents.hasEvent(0, PollEventFlags.POLLCOMPLETION))
                throw new AssertionError("native terminal was not drained by its public poller");
            if (!future.isDone()) throw new AssertionError("native completion was not delivered");
            drained++;
            if (cancellation) {
                if (!future.isCancelled())
                    throw new AssertionError("late reply replaced cancellation");
            } else {
                List<Message> parts = future.join();
                try {
                    if (parts.size() != 1 || !parts.getFirst().toUtf8String().equals("reply"))
                        throw new AssertionError("native reply changed");
                } finally {
                    Message.closeAll(parts);
                }
                if (!parts.getFirst().empty()) throw new AssertionError("caller reply leaked");
            }
        }

        @Override
        public void close() {
            completion.close();
            receive.close();
            caller.close();
            target.close();
            reply.close();
            payload.close();
            header.close();
            context.close();
        }
    }
}
