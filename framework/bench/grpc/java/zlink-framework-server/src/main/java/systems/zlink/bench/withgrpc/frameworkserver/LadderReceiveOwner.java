/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.bench.withgrpc.frameworkserver;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executors;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;
import systems.zlink.bench.withgrpc.shared.BenchServerMetrics;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue.Permit;

/** The L1 receive reservation, shared by every higher raw ladder level. */
final class LadderReceiveOwner {
    // ZLinkJavaRawMeshNode.MAX_INGRESS_BATCH{,_BYTES,_NANOS} (spec 08 §4.1).
    private static final int MAX_RECORDS = 64;
    private static final long MAX_BYTES = 4L * 1024 * 1024;
    private static final long MAX_NANOS = 2_000_000L;

    private final RouterSocket router;
    private final ZLinkApplicationJobQueue queue;
    private final Consumer<Received> handler;
    private final ExecutorService executor;
    private final Poller poller;
    private final PollEvents events = new PollEvents(1);
    private final AtomicReference<CompletableFuture<Permit>> pending = new AtomicReference<>();
    private final CompletableFuture<Void> stopped = new CompletableFuture<>();
    private final Received[] records = new Received[MAX_RECORDS];
    private final Permit[] permits = new Permit[MAX_RECORDS];
    private boolean reported;

    private LadderReceiveOwner(RouterSocket router, ZLinkApplicationJobQueue queue,
            Consumer<Received> handler, ExecutorService executor, Poller poller) {
        this.router = router;
        this.queue = queue;
        this.handler = handler;
        this.executor = executor;
        this.poller = poller;
    }

    static void run(RouterSocket router, ZLinkApplicationJobQueue queue,
            BenchServerMetrics metrics, Consumer<Received> handler) {
        try (var executor = Executors.newSingleThreadExecutor(
                        Thread.ofPlatform().name("bench-framework-ladder-receive").factory());
                var poller = Zlink.createPoller()) {
            poller.add(router, 0, PollEventFlags.POLLIN);
            var owner = new LadderReceiveOwner(router, queue, handler, executor, poller);
            try {
                executor.execute(() -> owner.turn(null));
                owner.stopped.join();
            } finally {
                ZLinkApplicationJobQueue.cancelPendingAcquire(owner.pending);
            }
        } catch (RuntimeException failure) {
            metrics.recordError();
            System.err.println("Ladder receive owner failed: " + failure);
        }
    }

    private void turn(Permit grant) {
        try {
            if (grant != null) drainRotation(grant);
            do {
                if (poller.wait(events, Duration.ofMillis(100)) > 0) drainRotation(null);
            } while (pending.get() == null);
            // Yield this executor to acquireOrResume's queued grant, as production pumpTurn does.
        } catch (ZlinkRecvException failure) {
            if (failure.getResult() == RecvResult.TERMINATED) stopped.complete(null);
            else stopped.completeExceptionally(failure);
        } catch (RuntimeException | Error failure) {
            stopped.completeExceptionally(failure);
        }
    }

    private void drainRotation(Permit grant) {
        final long startedAt = System.nanoTime();
        Permit next = grant;
        int budget = 0;
        int count = 0;
        long bytes = 0;
        int reserved = 0;
        try {
            // Same ordering as RawMeshNode.Rotation.acquirePermit, including resumed grants.
            reserved = budget = queue.reserveRotationBudget(MAX_RECORDS - (next == null ? 0 : 1));
            if (next == null) {
                if (budget > 0) {
                    next = queue.takeRotationPermit();
                    budget--;
                } else {
                    next = queue.acquireOrResume(executor, this::turn, pending,
                            ZLinkApplicationJobQueue.Origin.REMOTE);
                }
            }
            while (count < MAX_RECORDS
                    && (count == 0 || (bytes < MAX_BYTES
                            && System.nanoTime() - startedAt < MAX_NANOS))
                    && (next != null || budget > 0)) {
                // Java production splits before receiveNow, and returns the split on NO_DATA.
                if (next == null) {
                    next = queue.takeRotationPermit();
                    budget--;
                }
                Received received = new Received();
                boolean transferred = false;
                try {
                    try {
                        if (!router.recv(received, RecvFlags.DONT_WAIT)) break;
                    } catch (ZlinkRecvException failure) {
                        if (failure.getResult() == RecvResult.NO_DATA
                                || failure.getResult() == RecvResult.BUSY) break;
                        throw failure;
                    }
                    records[count] = received;
                    permits[count] = next;
                    next = null;
                    transferred = true;
                    count++;
                    for (var part : received.parts()) bytes = Math.addExact(bytes, part.size());
                } finally {
                    if (!transferred) received.close();
                }
            }
            // Production claims the rotation before dispatching its retained records.
            for (int index = 0; index < count; index++) {
                permits[index].handlerStarted();
                handler.accept(records[index]);
            }
        } finally {
            for (int index = 0; index < count; index++) {
                records[index].close();
                records[index] = null;
                permits[index].abandonReservation();
                permits[index] = null;
            }
            if (next != null) next.abandonReservation();
            if (budget > 0) queue.abandonRotationBudget(budget);
        }
        // One evidence line per process, outside the per-message path.
        if (!reported && count > 0) {
            reported = true;
            System.err.println("[ladder-permit] path=production-rotation receive_flow=registered"
                    + " reserved=" + reserved + " received=" + count
                    + " handler_started=" + count + " unused_returned=" + budget
                    + " permits_in_use=" + queue.snapshot().permitsInUse());
        }
    }
}
