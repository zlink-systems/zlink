package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.streams.ZLinkStreamRuntime;

import java.util.Collection;
import java.util.HashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkStreamShutdownBarrierTest {
    @Test
    void normalShutdownDrainsAcceptedStreamWorkOnce() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addStreamNode("shutdown-stream")
                .bind("tcp://127.0.0.1:0")
                .registerSession(ZLinkFrameworkRuntimeListenerStatusTest.NoOpSession.class);
        var snapshots = new AtomicInteger();
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var streamsField = ZLinkFrameworkRuntime.class.getDeclaredField("streams");
            streamsField.setAccessible(true);
            var streams = (ZLinkStreamRuntime) streamsField.get(runtime);
            var sessionsField = ZLinkStreamRuntime.class.getDeclaredField("sessions");
            sessionsField.setAccessible(true);
            // 빈 session registry로 실제 barrier 진입을 관찰한다. Runtime seam은 추가하지 않는다.
            sessionsField.set(
                    streams,
                    new HashMap<>() {
                        @Override
                        public Collection<Object> values() {
                            boolean barrier =
                                    StackWalker.getInstance()
                                            .walk(
                                                    frames ->
                                                            frames.anyMatch(
                                                                    frame ->
                                                                            frame.getClassName()
                                                                                            .equals(
                                                                                                    ZLinkStreamRuntime
                                                                                                            .class
                                                                                                            .getName())
                                                                                    && frame.getMethodName()
                                                                                            .contains(
                                                                                                    "awaitDrainBarrier")));
                            if (barrier) snapshots.incrementAndGet();
                            return super.values();
                        }
                    });
            var result = runtime.shutdown().toCompletableFuture().get(10, TimeUnit.SECONDS);
            assertEquals(ZLinkFrameworkTerminationOutcome.STOPPED, result.outcome());
            assertEquals(ZLinkFrameworkTerminationReason.NONE, result.reason());
            assertEquals(1, snapshots.get(), "accepted STREAM barrier snapshots");
        }
    }
}
