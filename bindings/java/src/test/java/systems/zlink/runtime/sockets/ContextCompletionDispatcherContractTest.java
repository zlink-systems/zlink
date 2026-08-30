/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import java.lang.foreign.MemorySegment;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.runtime.nativeapi.CompletionDispatcher;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;

class ContextCompletionDispatcherContractTest {
    @Test
    void borrowedRawHandleIsRejectedBeforeCallbackOwnership() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext()) {
            MemorySegment rawHandle = Native.socket(
                InternalAccess.contextHandle(context),
                SocketType.PAIR.getValue());
            assertTrue(rawHandle != null && rawHandle.address() != 0L);
            try {
                IllegalArgumentException failure = assertThrows(
                    IllegalArgumentException.class,
                    () -> new NativeSocketRuntime(rawHandle, false,
                        SocketType.PAIR));
                assertEquals("borrowed raw socket handles are not supported",
                    failure.getMessage());
            } finally {
                assertEquals(0, Native.close(rawHandle),
                    "rejection must leave the borrowed handle with its owner");
            }
        }
    }

    @Test
    void socketsShareBoundedContextCompletionWorkersOwnedByContext()
        throws Exception {
        TestSupport.assumeNative();

        Context context = Zlink.createContext();
        List<NativeSocketRuntime> sockets = new ArrayList<>();
        Set<Thread> workers = ConcurrentHashMap.newKeySet();
        try {
            int workerLimit = InternalAccess
                .contextCompletionDispatcher(context).workerLimit();
            assertEquals(Math.min(16,
                Math.max(1, Runtime.getRuntime().availableProcessors())),
                workerLimit);
            int socketCount = workerLimit + 2;
            CountDownLatch completed = new CountDownLatch(socketCount);
            for (int i = 0; i < socketCount; i++) {
                NativeSocketRuntime socket = new NativeSocketRuntime(context,
                    SocketType.PAIR);
                sockets.add(socket);
                socket.dispatchCompletion(() -> {
                    Thread current = Thread.currentThread();
                    workers.add(current);
                    completed.countDown();
                });
            }

            assertTrue(completed.await(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS));
            assertEquals(workerLimit, workers.size(),
                "one context must use only its bounded completion workers");
            assertTrue(workers.size() < socketCount,
                "completion workers must not scale with socket count");
            assertTrue(workers.stream().allMatch(thread ->
                thread.getName().equals("zlink-send-completion")));
            assertTrue(workers.stream().allMatch(Thread::isDaemon));

            for (int i = 0; i + 1 < sockets.size(); i++) {
                sockets.get(i).close();
            }
            CountDownLatch afterSocketClose = new CountDownLatch(1);
            sockets.getLast().dispatchCompletion(afterSocketClose::countDown);
            assertTrue(afterSocketClose.await(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS),
                "closing one socket must not close the context dispatcher");
        } finally {
            try {
                for (NativeSocketRuntime socket : sockets) {
                    try {
                        socket.close();
                    } catch (RuntimeException ignored) {
                    }
                }
            } finally {
                context.close();
            }
        }

        assertFalse(workers.isEmpty());
        for (Thread completionWorker : workers) {
            completionWorker.join(TestSupport.DEFAULT_TIMEOUT_MS);
            assertFalse(completionWorker.isAlive(),
                "context close must release every completion worker");
        }
    }

    @Test
    void differentSocketsCanRunCompletionsConcurrently() throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             NativeSocketRuntime first = new NativeSocketRuntime(context,
                 SocketType.PAIR);
             NativeSocketRuntime second = new NativeSocketRuntime(context,
                 SocketType.PAIR)) {
            assumeTrue(InternalAccess.contextCompletionDispatcher(context)
                .workerLimit() >= 2);
            CountDownLatch bothStarted = new CountDownLatch(2);
            CountDownLatch release = new CountDownLatch(1);
            CountDownLatch finished = new CountDownLatch(2);
            Runnable blockingCompletion = () -> {
                bothStarted.countDown();
                try {
                    release.await(TestSupport.DEFAULT_TIMEOUT_MS,
                        TimeUnit.MILLISECONDS);
                } catch (InterruptedException failure) {
                    Thread.currentThread().interrupt();
                } finally {
                    finished.countDown();
                }
            };

            first.dispatchCompletion(blockingCompletion);
            second.dispatchCompletion(blockingCompletion);
            try {
                assertTrue(bothStarted.await(TestSupport.DEFAULT_TIMEOUT_MS,
                    TimeUnit.MILLISECONDS),
                    "one blocked socket must not stall another socket");
            } finally {
                release.countDown();
            }
            assertTrue(finished.await(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS));
        }
    }

    @Test
    void oneSocketRetainsSerialCompletionOrder() throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             NativeSocketRuntime socket = new NativeSocketRuntime(context,
                 SocketType.PAIR)) {
            CountDownLatch firstStarted = new CountDownLatch(1);
            CountDownLatch releaseFirst = new CountDownLatch(1);
            CountDownLatch secondStarted = new CountDownLatch(1);
            socket.dispatchCompletion(() -> {
                firstStarted.countDown();
                try {
                    releaseFirst.await(TestSupport.DEFAULT_TIMEOUT_MS,
                        TimeUnit.MILLISECONDS);
                } catch (InterruptedException failure) {
                    Thread.currentThread().interrupt();
                }
            });
            assertTrue(firstStarted.await(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS));

            socket.dispatchCompletion(secondStarted::countDown);
            try {
                assertFalse(secondStarted.await(100L, TimeUnit.MILLISECONDS),
                    "one socket's later completion must stay on its lane");
            } finally {
                releaseFirst.countDown();
            }
            assertTrue(secondStarted.await(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS));
        }
    }

    @Test
    void rawHandleCloseDrainsQueuedCompletionAndReleasesWorker()
        throws Exception {
        TestSupport.assumeNative();

        Context context = Zlink.createContext();
        MemorySegment rawHandle = Native.socket(
            InternalAccess.contextHandle(context), SocketType.PAIR.getValue());
        assertTrue(rawHandle != null && rawHandle.address() != 0L);
        NativeSocketRuntime socket = null;
        AtomicReference<Thread> worker = new AtomicReference<>();
        CountDownLatch firstStarted = new CountDownLatch(1);
        CountDownLatch releaseFirst = new CountDownLatch(1);
        CountDownLatch drained = new CountDownLatch(1);
        try {
            socket = new NativeSocketRuntime(rawHandle, true, SocketType.PAIR);
            socket.dispatchCompletion(() -> {
                worker.set(Thread.currentThread());
                firstStarted.countDown();
                try {
                    releaseFirst.await(TestSupport.DEFAULT_TIMEOUT_MS,
                        TimeUnit.MILLISECONDS);
                } catch (InterruptedException failure) {
                    Thread.currentThread().interrupt();
                }
            });
            assertTrue(firstStarted.await(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS));
            socket.dispatchCompletion(drained::countDown);
            socket.close();
            assertEquals(1L, drained.getCount(),
                "close must not skip a queued lane completion");
            releaseFirst.countDown();
            assertTrue(drained.await(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS));
        } finally {
            releaseFirst.countDown();
            try {
                if (socket != null) {
                    socket.close();
                } else {
                    Native.close(rawHandle);
                }
            } finally {
                context.close();
            }
        }

        Thread completionWorker = worker.get();
        assertNotNull(completionWorker);
        completionWorker.join(TestSupport.DEFAULT_TIMEOUT_MS);
        assertFalse(completionWorker.isAlive(),
            "a raw-handle socket must release its owned dispatcher");
    }

    @Test
    void closedDispatcherUsesOffCallerSerialFallback() throws Exception {
        CompletionDispatcher dispatcher = new CompletionDispatcher(
            "zlink-rejected-completion", 1);
        CompletionDispatcher.CompletionLane lane = dispatcher.acquireLane();
        dispatcher.close();

        Thread caller = Thread.currentThread();
        AtomicReference<Thread> completionThread = new AtomicReference<>();
        CountDownLatch completed = new CountDownLatch(1);
        lane.dispatch(() -> {
            completionThread.set(Thread.currentThread());
            completed.countDown();
        });

        assertTrue(completed.await(TestSupport.DEFAULT_TIMEOUT_MS,
            TimeUnit.MILLISECONDS));
        assertNotNull(completionThread.get());
        assertFalse(completionThread.get() == caller,
            "rejection must not run completion on the callback caller");
        assertTrue(completionThread.get().isVirtual());
        assertEquals("zlink-rejected-completion-teardown",
            completionThread.get().getName());
    }

}
