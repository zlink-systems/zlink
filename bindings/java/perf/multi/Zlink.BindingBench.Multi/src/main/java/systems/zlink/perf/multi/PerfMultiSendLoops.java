/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;



import systems.zlink.perf.PerfUtil;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;

final class PerfMultiSendLoops {
    private PerfMultiSendLoops() {
    }

    static void runClients(int clientCount, ClientFactory factory,
                           int durationSeconds) {
        List<Thread> threads = new ArrayList<>(clientCount);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        for (int i = 0; i < clientCount; i++) {
            Thread thread = factory.create(i, durationSeconds);
            thread.setUncaughtExceptionHandler((current, error) ->
                failure.compareAndSet(null, error));
            threads.add(thread);
            thread.start();
        }
        for (int i = 0; i < threads.size(); i++) {
            PerfUtil.join(threads.get(i), "multi client " + i, Duration.ofSeconds(
                durationSeconds + 30L));
        }
        Throwable error = failure.get();
        if (error != null) {
            throw new IllegalStateException("multi client failed", error);
        }
    }

    interface ClientFactory {
        Thread create(int index, int durationSeconds);
    }
}
