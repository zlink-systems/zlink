package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.DealerSocket;

final class ZLinkJavaSocketMonitorTest {
    @Test
    void receiveSurvivesAnEmptyMonitorQueueAndEndsWhenClosed() throws Exception {
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket()) {
            ZLinkJavaSocketMonitor monitor = new ZLinkJavaSocketMonitor(
                dealer.monitorOpen());
            CompletableFuture<?> receive = CompletableFuture.supplyAsync(
                monitor::recv);
            try {
                // The native monitor's default receive timeout is bounded.
                // Its first NO_DATA must not terminate the Framework drain.
                Thread.sleep(1_100);
                assertFalse(receive.isDone());

                monitor.close();
                assertThrows(ExecutionException.class,
                    () -> receive.get(2, TimeUnit.SECONDS));
            } finally {
                monitor.close();
                receive.cancel(true);
            }
        }
    }

    @Test
    void receiveEndsWhenItsDrainThreadIsInterrupted() throws Exception {
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket()) {
            ZLinkJavaSocketMonitor monitor = new ZLinkJavaSocketMonitor(
                dealer.monitorOpen());
            ExecutorService executor = Executors.newSingleThreadExecutor();
            try {
                var receive = executor.submit(monitor::recv);
                Thread.sleep(100);
                receive.cancel(true);
                executor.shutdown();
                assertTrue(executor.awaitTermination(2, TimeUnit.SECONDS));
            } finally {
                monitor.close();
                executor.shutdownNow();
            }
        }
    }
}
