/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;

final class ContextCloseContractTest {
    @Test
    void closeCallsTermOnceAfterShutdown() throws Exception {
        TestSupport.assumeNative();
        CompletionNativeFixture.runProbe(TermProbe.class);
    }

    @Test
    void concurrentCloseCallsTermOnce() throws Exception {
        TestSupport.assumeNative();
        CompletionNativeFixture.runProbe(ConcurrentTermProbe.class);
    }

    public static final class ConcurrentTermProbe {
        public static void main(String[] args) throws Throwable {
            CompletionNativeFixture core = new CompletionNativeFixture();
            core.ctxTermDelayMs = 2;
            core.ctxTermForwardFirstOnly = true;
            int threads = 16;
            for (int i = 0; i < 100; i++) {
                Context context = Zlink.createContext();
                int before = core.ctxTermCalls.get();
                java.util.concurrent.CyclicBarrier start =
                    new java.util.concurrent.CyclicBarrier(threads);
                java.util.concurrent.atomic.AtomicReference<Throwable> error =
                    new java.util.concurrent.atomic.AtomicReference<>();
                Thread[] workers = new Thread[threads];
                for (int t = 0; t < threads; t++) {
                    workers[t] = new Thread(() -> {
                        try {
                            start.await();
                            context.close();
                        } catch (Throwable ex) {
                            error.compareAndSet(null, ex);
                        }
                    });
                    workers[t].start();
                }
                for (Thread worker : workers)
                    worker.join();
                assertNull(error.get());
                assertEquals(1, core.ctxTermCalls.get() - before);
                context.close();
                assertEquals(1, core.ctxTermCalls.get() - before);
            }
            System.exit(0);
        }
    }

    public static final class TermProbe {
        public static void main(String[] args) throws Throwable {
            CompletionNativeFixture core = new CompletionNativeFixture();
            Context context = Zlink.createContext();
            core.ctxTermInterrupted.set(1);
            ZlinkCloseException failure = assertThrows(ZlinkCloseException.class,
                context::close);
            assertEquals(CloseResult.INTERNAL_ERROR, failure.getResult());
            assertEquals(1, core.ctxTermCalls.get());
            System.exit(0);
        }
    }
}
