package systems.zlink.samples.deliverydispatch.server.dispatch;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneId;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorRequestCall;
import systems.zlink.framework.actors.ZLinkActorSendCall;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleTimings;
import systems.zlink.samples.deliverydispatch.server.dispatch.DeliveryOfferStore.DeliveryOffer;
import systems.zlink.samples.deliverydispatch.shared.contracts.Messages;

final class DispatchWorkerDeadlineTest {
    @Test
    void firstOfferDeadlineStartsAtCourierSendAfterSlowStatusPublish() throws Exception {
        assertDeadlineStartsAtSend(false);
    }

    @Test
    void reassignedOfferDeadlineStartsAtCourierSendAfterSlowStatusPublish() throws Exception {
        assertDeadlineStartsAtSend(true);
    }

    private static void assertDeadlineStartsAtSend(boolean reassign) throws Exception {
        CompletableFuture<Messages.DeliveryStatusChangedRes> statusReply = new CompletableFuture<>();
        MutableClock clock = new MutableClock(Instant.parse("2026-09-05T00:00:00Z"));
        AtomicReference<Instant> sendTime = new AtomicReference<>();
        DeliveryOfferStore offers = new DeliveryOfferStore(clock);
        DispatchWorker worker = new DispatchWorker(
            delayedStatusClient(statusReply), recordingActorClient(sendTime, clock), offers);
        Messages.AssignDeliveryMsg request = new Messages.AssignDeliveryMsg(
            reassign ? "delivery-reassign" : "delivery-first",
            "customer-1",
            "pickup",
            "dropoff");

        CompletionStage<Void> operation;
        int expectedAttempt;
        if (reassign) {
            int firstAttempt = offers.offer(request, 0, Duration.ofMinutes(1));
            DeliveryOffer firstOffer = offers.settle(request.deliveryId(), firstAttempt).orElseThrow();
            operation = worker.reassign(firstOffer);
            expectedAttempt = 2;
        } else {
            operation = worker.dispatch(request);
            expectedAttempt = 1;
        }

        clock.advance(SampleTimings.CourierDecisionTimeout.plusMillis(100));
        assertTrue(offers.takeExpired().isEmpty(),
            "status publication time must not consume the courier decision timeout");
        assertTrue(sendTime.get() == null, "the courier send must wait for status publication");

        statusReply.complete(new Messages.DeliveryStatusChangedRes(
            request.deliveryId(),
            reassign ? Messages.DeliveryStatus.Reassigned : Messages.DeliveryStatus.Assigned));
        operation.toCompletableFuture().get(2, TimeUnit.SECONDS);

        Instant sentAt = sendTime.get();
        assertFalse(sentAt == null, "the courier offer must be sent after status publication");
        DeliveryOffer active = offers.settle(request.deliveryId(), expectedAttempt).orElseThrow();
        long remainingAtSend = Duration.between(sentAt, active.deadline()).toMillis();
        assertEquals(expectedAttempt, active.attempt());
        assertTrue(remainingAtSend >= SampleTimings.CourierDecisionTimeout.toMillis() - 50,
            "the deadline must retain the courier's full response interval at send time");
        assertTrue(remainingAtSend <= SampleTimings.CourierDecisionTimeout.toMillis());
    }

    private static ZLinkClient delayedStatusClient(
        CompletableFuture<Messages.DeliveryStatusChangedRes> statusReply) {
        return new ZLinkClient() {
            @Override
            public ZLinkSendCall sendToChannel(String channelName, Object message) {
                throw new UnsupportedOperationException();
            }

            @Override
            public ZLinkRequestCall requestToChannel(String channelName, Object message) {
                return new ZLinkRequestCall() {
                    @Override
                    public ZLinkRequestCall timeout(Duration timeout) {
                        return this;
                    }

                    @Override
                    @SuppressWarnings("unchecked")
                    public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
                        return (CompletionStage<TReply>) statusReply;
                    }

                    @Override
                    public <TReply> CompletionStage<TReply> yield(Class<TReply> replyType) {
                        throw new UnsupportedOperationException();
                    }
                };
            }
        };
    }

    private static ZLinkActorClient recordingActorClient(
        AtomicReference<Instant> sendTime,
        Clock clock) {
        return new ZLinkActorClient() {
            @Override
            public ZLinkActorSendCall sendToActor(String actorId, Object message) {
                sendTime.set(clock.instant());
                return new ZLinkActorSendCall() {
                    @Override
                    public ZLinkActorSendCall metadata(String key, String value) {
                        return this;
                    }

                    @Override
                    public CompletionStage<Void> submit() {
                        return CompletableFuture.completedFuture(null);
                    }
                };
            }

            @Override
            public ZLinkActorRequestCall requestToActor(String actorId, Object request) {
                throw new UnsupportedOperationException();
            }
        };
    }

    private static final class MutableClock extends Clock {
        private Instant current;

        private MutableClock(Instant current) {
            this.current = current;
        }

        private void advance(Duration duration) {
            current = current.plus(duration);
        }

        @Override
        public ZoneId getZone() {
            return ZoneId.of("UTC");
        }

        @Override
        public Clock withZone(ZoneId zone) {
            return Clock.fixed(current, zone);
        }

        @Override
        public Instant instant() {
            return current;
        }
    }
}
