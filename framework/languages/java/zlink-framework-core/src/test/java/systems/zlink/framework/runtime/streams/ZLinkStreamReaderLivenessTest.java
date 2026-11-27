package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import static systems.zlink.framework.runtime.streams.ZLinkStreamRuntimeIngressTest.PEER_A;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.eventing.MonitorEventType;

import java.time.Duration;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

class ZLinkStreamReaderLivenessTest {
    @ParameterizedTest(name = "complete ping = {0}")
    @ValueSource(booleans = {true, false})
    void inboundDeadlineUsesCompleteFramesAvailableToReader(boolean complete) throws Exception {
        var fixture = new ZLinkStreamRuntimeIngressTest();
        var stream = new ZLinkStreamRuntimeIngressTest.FakeStream();
        var clock = new AtomicLong();
        var pollEntered = new CountDownLatch(1);
        var releasePoll = new CountDownLatch(1);
        stream.readabilityObserver =
                () -> {
                    pollEntered.countDown();
                    try {
                        assertTrue(releasePoll.await(5, TimeUnit.SECONDS));
                    } catch (InterruptedException failure) {
                        Thread.currentThread().interrupt();
                        throw new AssertionError(failure);
                    }
                };
        var closedReasons = new CopyOnWriteArrayList<String>();
        try (var metrics = ZLinkStreamRuntimeIngressTest.installClosedMetricSink(closedReasons)) {
            var runtime = fixture.startDeterministic(stream, clock);
            try {
                assertTrue(pollEntered.await(5, TimeUnit.SECONDS));
                stream.errorHandler.handle(PEER_A, MonitorEventType.CONNECTION_READY, 0, "ready");
                byte[] ping =
                        ZLinkStreamFrameCodec.encode(ZLinkStreamHeader.HEARTBEAT_PING, new byte[0]);
                byte[] arrived = complete ? ping : Arrays.copyOf(ping, ping.length - 1);
                // The PACKET backend only makes complete frames readable.
                ZLinkStreamFrameCodec.tryDecode(arrived)
                        .ifPresent(frame -> stream.enqueue(PEER_A, arrived));
                clock.set(Duration.ofSeconds(5).toNanos());
                fixture.fireLivenessTimers(runtime);

                if (complete) {
                    assertEquals(0, stream.sessionClosingSends.get(), closedReasons::toString);
                    releasePoll.countDown();
                    assertTrue(stream.heartbeatPongAsyncAttempted.await(5, TimeUnit.SECONDS));
                    stream.awaitReaderProgress();
                    assertEquals(List.of(), closedReasons);
                } else {
                    releasePoll.countDown();
                    stream.awaitReaderProgress();
                    assertEquals(1, stream.sessionClosingSends.get());
                    assertEquals(List.of("heartbeat_timeout"), closedReasons);
                }
            } finally {
                releasePoll.countDown();
                fixture.resetSessionProbe();
            }
        }
    }
}
