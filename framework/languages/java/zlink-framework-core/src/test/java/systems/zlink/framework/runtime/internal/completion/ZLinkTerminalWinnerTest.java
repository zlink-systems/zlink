package systems.zlink.framework.runtime.internal.completion;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;

import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;

final class ZLinkTerminalWinnerTest {
    @Test
    void exactlyOneTerminalCauseWinsConcurrentCompletion() throws Exception {
        ZLinkTerminalWinner winner = new ZLinkTerminalWinner();
        ZLinkTerminalWinner.Cause[] causes =
            ZLinkTerminalWinner.Cause.values();
        CountDownLatch ready = new CountDownLatch(causes.length);
        CountDownLatch start = new CountDownLatch(1);
        AtomicInteger wins = new AtomicInteger();
        try (var executor = Executors.newVirtualThreadPerTaskExecutor()) {
            Arrays.stream(causes).forEach(cause -> executor.submit(() -> {
                ready.countDown();
                start.await();
                if (winner.tryWin(cause)) {
                    wins.incrementAndGet();
                }
                return null;
            }));
            ready.await(3, TimeUnit.SECONDS);
            start.countDown();
        }

        assertEquals(1, wins.get());
        assertNotNull(winner.winner());
    }
}
