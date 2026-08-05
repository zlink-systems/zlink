package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.function.Function;
import java.util.function.IntConsumer;
import java.util.function.IntUnaryOperator;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;

class ZLinkJavaMeshDispatchPumpTest {
    @Test
    void callbackOnlySchedulesAndResidueIsDrainedSerially() throws Exception {
        RecordingSource source = new RecordingSource();
        CountDownLatch drained = new CountDownLatch(2);
        ExecutorService executor = Executors.newSingleThreadExecutor();

        try (ZLinkJavaMeshDispatchPump pump =
                 new ZLinkJavaMeshDispatchPump(source, ignored -> {
                     drained.countDown();
                     return CompletableFuture.completedFuture(null);
                 }, executor)) {
            assertEquals(3, source.readyHandler.applyAsInt(3));
            assertTrue(drained.await(2, TimeUnit.SECONDS));
            assertEquals(List.of(3, 3), source.domains);
            assertEquals(1, source.maxConcurrentDrains);
        }

        assertTrue(source.closed);
    }

    @Test
    void callbacksQueuedBeforeThePumpRunsAreCoalescedByDomain() throws Exception {
        RecordingSource source = new RecordingSource();
        source.first = false;
        CountDownLatch releaseExecutor = new CountDownLatch(1);
        CountDownLatch drained = new CountDownLatch(1);
        ExecutorService executor = Executors.newSingleThreadExecutor();
        executor.execute(() -> {
            try {
                releaseExecutor.await();
            } catch (InterruptedException interruption) {
                Thread.currentThread().interrupt();
            }
        });

        try (ZLinkJavaMeshDispatchPump pump =
                 new ZLinkJavaMeshDispatchPump(source, ignored -> {
                     drained.countDown();
                     return CompletableFuture.completedFuture(null);
                 }, executor)) {
            assertEquals(1, source.readyHandler.applyAsInt(1));
            assertEquals(2, source.readyHandler.applyAsInt(2));
            releaseExecutor.countDown();
            assertTrue(drained.await(2, TimeUnit.SECONDS));
            assertEquals(List.of(3), source.domains);
        }
    }

    @Test
    void residueIsRescheduledAfterTheElapsedTurnBudget() throws Exception {
        TimeCappedSource source = new TimeCappedSource();
        CountDownLatch drained = new CountDownLatch(6);
        AtomicInteger executions = new AtomicInteger();
        ThreadPoolExecutor executor = new ThreadPoolExecutor(
            1,
            1,
            0,
            TimeUnit.MILLISECONDS,
            new LinkedBlockingQueue<>()) {
            @Override
            public void execute(Runnable command) {
                super.execute(() -> {
                    executions.incrementAndGet();
                    command.run();
                });
            }
        };

        try (ZLinkJavaMeshDispatchPump pump =
                 new ZLinkJavaMeshDispatchPump(source, ignored -> {
                     drained.countDown();
                     return CompletableFuture.completedFuture(null);
                 }, executor)) {
            source.readyHandler.applyAsInt(1);

            assertTrue(drained.await(2, TimeUnit.SECONDS));
            assertEquals(6, source.drains.get());
            assertTrue(executions.get() >= 2,
                "residue was not split across dispatch turns");
        }
    }

    private static final class RecordingSource implements ZLinkJavaMeshDispatchPump.Source {
        private final List<Integer> domains = new ArrayList<>();
        private IntUnaryOperator readyHandler;
        private int activeDrains;
        private int maxConcurrentDrains;
        private boolean first = true;
        private boolean closed;

        @Override
        public void setReadyHandler(IntUnaryOperator handler) {
            readyHandler = handler;
        }

        @Override
        public synchronized boolean drain(
            int readyDomains,
            Function<ZLinkMeshDispatchRecord, CompletionStage<Void>> receiver,
            IntConsumer released) {
            activeDrains++;
            maxConcurrentDrains = Math.max(maxConcurrentDrains, activeDrains);
            try {
                domains.add(readyDomains);
                receiver.apply(null);
                if (first) {
                    first = false;
                    return true;
                }
                return false;
            } finally {
                activeDrains--;
            }
        }

        @Override
        public void close() {
            closed = true;
        }
    }

    private static final class TimeCappedSource
        implements ZLinkJavaMeshDispatchPump.Source {
        private final AtomicInteger drains = new AtomicInteger();
        private IntUnaryOperator readyHandler;

        @Override
        public void setReadyHandler(IntUnaryOperator handler) {
            readyHandler = handler;
        }

        @Override
        public boolean drain(
            int readyDomains,
            Function<ZLinkMeshDispatchRecord, CompletionStage<Void>> receiver,
            IntConsumer released) {
            int count = drains.incrementAndGet();
            receiver.apply(null);
            if (count == 1) {
                try {
                    Thread.sleep(20);
                } catch (InterruptedException interruption) {
                    Thread.currentThread().interrupt();
                }
            }
            return count < 6;
        }

        @Override
        public void close() {
        }
    }
}
