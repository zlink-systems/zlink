package systems.zlink.framework.runtime.internal.service;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;

import java.io.IOException;
import java.lang.classfile.ClassFile;
import java.lang.classfile.ClassTransform;
import java.lang.classfile.instruction.InvokeInstruction;
import java.lang.constant.ClassDesc;
import java.lang.constant.MethodTypeDesc;
import java.lang.reflect.InvocationTargetException;
import java.time.Duration;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/** Isolates maintenance wakeups from real timer scheduling and elapsed wall time. */
public final class ZLinkServiceOperationRegistryWakeTest {
    @Test
    void serialRequestsRetainTheReservedVisitAndIdleDoesNotPoll() throws Throwable {
        // Rewrite only the isolated copy's waits. The real registry's registration,
        // completion, wheel, wakeup and lifecycle code executes unchanged.
        ClassLoader isolated =
                new ClassLoader(getClass().getClassLoader()) {
                    @Override
                    protected Class<?> loadClass(String name, boolean resolve)
                            throws ClassNotFoundException {
                        if (!name.startsWith("systems.zlink.framework.runtime.internal.service."))
                            return super.loadClass(name, resolve);
                        synchronized (getClassLoadingLock(name)) {
                            Class<?> type = findLoadedClass(name);
                            if (type == null) {
                                try (var source =
                                        getResourceAsStream(name.replace('.', '/') + ".class")) {
                                    if (source == null) throw new ClassNotFoundException(name);
                                    byte[] bytes = source.readAllBytes();
                                    if (name.equals(
                                            ZLinkServiceOperationRegistry.class.getName())) {
                                        var file = ClassFile.of();
                                        ClassDesc helper =
                                                ClassDesc.of(
                                                        ZLinkServiceOperationRegistryWakeTest.class
                                                                .getName());
                                        bytes =
                                                file.transformClass(
                                                        file.parse(bytes),
                                                        ClassTransform.transformingMethodBodies(
                                                                method ->
                                                                        method.methodName()
                                                                                .stringValue()
                                                                                .contains(
                                                                                        "maintainDeadlines"),
                                                                (builder, element) -> {
                                                                    if (element
                                                                                    instanceof
                                                                                    InvokeInstruction
                                                                                            call
                                                                            && call.name()
                                                                                    .equalsString(
                                                                                            "parkNanos")) {
                                                                        builder.invokestatic(
                                                                                helper,
                                                                                "parkNanos",
                                                                                MethodTypeDesc
                                                                                        .ofDescriptor(
                                                                                                "(Ljava/lang/Object;J)V"));
                                                                    } else if (element
                                                                                    instanceof
                                                                                    InvokeInstruction
                                                                                            call
                                                                            && call.name()
                                                                                    .equalsString(
                                                                                            "park")) {
                                                                        builder.invokestatic(
                                                                                helper,
                                                                                "park",
                                                                                MethodTypeDesc
                                                                                        .ofDescriptor(
                                                                                                "(Ljava/lang/Object;)V"));
                                                                    } else if (element
                                                                                    instanceof
                                                                                    InvokeInstruction
                                                                                            call
                                                                            && call.name()
                                                                                    .equalsString(
                                                                                            "expireOnOwner")) {
                                                                        builder.invokestatic(
                                                                                helper,
                                                                                "expire",
                                                                                MethodTypeDesc
                                                                                        .ofDescriptor(
                                                                                                "(Lsystems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry;J)I"));
                                                                    } else builder.with(element);
                                                                }));
                                    }
                                    type = defineClass(name, bytes, 0, bytes.length);
                                } catch (IOException failure) {
                                    throw new ClassNotFoundException(name, failure);
                                }
                            }
                            if (resolve) resolveClass(type);
                            return type;
                        }
                    }
                };
        try {
            isolated.loadClass(getClass().getName()).getMethod("runScenario").invoke(null);
        } catch (InvocationTargetException failure) {
            throw failure.getCause();
        }
    }

    static final AtomicInteger returns = new AtomicInteger();
    static final AtomicInteger expires = new AtomicInteger();
    static final AtomicReference<Thread> worker = new AtomicReference<>();
    static Object blocker;

    public static void parkNanos(Object owner, long remaining) {
        if (remaining <= 0) throw new AssertionError("nonpositive timed wait");
        java.util.concurrent.locks.LockSupport.park(owner);
        returns.incrementAndGet();
    }

    public static void park(Object owner) {
        java.util.concurrent.locks.LockSupport.park(owner);
        returns.incrementAndGet();
    }

    public static int expire(ZLinkServiceOperationRegistry registry, long now) {
        expires.incrementAndGet();
        return ZLinkCompletionBridge.await(registry.expire(now));
    }

    static void parked() {
        long guard = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (true) {
            if (worker.get().getState() == Thread.State.WAITING
                    && java.util.concurrent.locks.LockSupport.getBlocker(worker.get()) == blocker)
                return;
            if (System.nanoTime() - guard >= 0) throw new AssertionError("worker did not park");
            Thread.yield();
        }
    }

    private static void awaitReturns(int minimum) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (returns.get() < minimum) {
            if (System.nanoTime() - deadline >= 0) throw new AssertionError("worker did not wake");
            Thread.yield();
        }
    }

    public static void runScenario() throws Exception {
        AtomicLong clock = new AtomicLong();
        var registry =
                new ZLinkServiceOperationRegistry(
                        command -> {
                            Thread thread = new Thread(command, "deterministic-deadline-worker");
                            if (!worker.compareAndSet(null, thread))
                                throw new AssertionError("multiple workers");
                            return thread;
                        },
                        new IllegalStateException("closed"),
                        clock::get);
        blocker = registry;
        int serialReturns;
        try (registry) {
            var operation =
                    ZLinkCompletionBridge.await(registry.<Integer>register(Duration.ofMillis(100)));
            parked();
            int before = returns.get();
            for (int request = 0; request < 1_000; request++) {
                if (request != 0)
                    operation =
                            ZLinkCompletionBridge.await(registry.register(Duration.ofMillis(100)));
                if (!registry.complete(operation.id(), request))
                    throw new AssertionError("reply lost");
                if (operation.get(5, TimeUnit.SECONDS) != request)
                    throw new AssertionError("wrong reply");
                parked();
            }
            serialReturns = returns.get() - before;
            System.out.printf(
                    "serial requests=1000 completed=1000 wait_returns=%d expire_calls=%d"
                            + " upper_bound=2%n",
                    serialReturns, expires.get());
            // Let the already reserved visit occur, then observe indefinite idle.
            int beforeVisit = returns.get();
            clock.set(TimeUnit.MILLISECONDS.toNanos(100));
            java.util.concurrent.locks.LockSupport.unpark(worker.get());
            long guard = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
            while (registry.maintenanceTickForTest() >= 0) {
                if (System.nanoTime() - guard >= 0) throw new AssertionError("wheel did not drain");
                Thread.yield();
            }
            awaitReturns(beforeVisit + 2);
            parked();
            int idleBefore = returns.get();
            int expiryBefore = expires.get();
            clock.addAndGet(TimeUnit.SECONDS.toNanos(5));
            parked();
            System.out.printf(
                    "idle virtual_seconds=5 wait_returns=%d expire_calls=%d%n",
                    returns.get() - idleBefore, expires.get() - expiryBefore);
            if (returns.get() != idleBefore || expires.get() != expiryBefore)
                throw new AssertionError("idle polling");
            // The first registration wakes indefinite wait. Completing it leaves its timed visit.
            var later = ZLinkCompletionBridge.await(registry.register(Duration.ofSeconds(30)));
            awaitReturns(idleBefore + 1);
            parked();
            if (returns.get() - idleBefore != 1)
                throw new AssertionError("long deadline must wake the idle owner once");
            System.out.printf(
                    "long_deadline seconds=30 wait_returns=%d%n", returns.get() - idleBefore);
            if (!registry.discard(later.id())) throw new AssertionError("discard failed");
            var lane = registry.ownerLaneForTest();
            int beforeEarlier = returns.get();
            var earlier =
                    lane.awaitTurn(
                            () -> {
                                var request =
                                        ZLinkCompletionBridge.await(
                                                registry.register(Duration.ofNanos(1)));
                                // The transformed park has no real timeout. Exercise the
                                // worker parking before virtual time advances, then wake it.
                                awaitReturns(beforeEarlier + 1);
                                parked();
                                clock.addAndGet(TimeUnit.MILLISECONDS.toNanos(2));
                                return request;
                            });
            java.util.concurrent.locks.LockSupport.unpark(worker.get());
            try {
                earlier.get(5, TimeUnit.SECONDS);
                throw new AssertionError("deadline did not expire");
            } catch (ExecutionException failure) {
                if (!(failure.getCause() instanceof TimeoutException)) throw failure;
            }
            parked();
            System.out.printf("earlier_deadline expired=true expire_calls=%d%n", expires.get());
            registry.close();
            registry.close();
            if (worker.get().isAlive()) throw new AssertionError("worker not joined");
        }
        if (serialReturns > 2)
            throw new AssertionError("serial wait returns=" + serialReturns + " > 2");
        queuedVisitDoesNotMultiplyOnRegistrationWakeups();
        reservedEmptyVisitRunsOnce();
        serialRequestsAcrossRotations();
        System.out.println("PASS");
    }

    private static void reservedEmptyVisitRunsOnce() throws Exception {
        worker.set(null);
        AtomicLong clock = new AtomicLong();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        command -> {
                            Thread thread = new Thread(command, "empty-reserved-visit");
                            worker.set(thread);
                            return thread;
                        },
                        new IllegalStateException("closed"),
                        clock::get)) {
            blocker = registry;
            var request = ZLinkCompletionBridge.await(registry.register(Duration.ofSeconds(30)));
            parked();
            if (!registry.complete(request.id(), 1)) throw new AssertionError("reply lost");
            request.get(5, TimeUnit.SECONDS);
            // Drain stale entries before the worker's booked visit, without changing its
            // reservation.
            clock.set(TimeUnit.MILLISECONDS.toNanos(1));
            ZLinkCompletionBridge.await(registry.expire(clock.get()));
            if (registry.maintenanceTickForTest() != 256)
                throw new AssertionError("reserved empty visit cancelled");
            int before = expires.get();
            clock.set(TimeUnit.MILLISECONDS.toNanos(256));
            java.util.concurrent.locks.LockSupport.unpark(worker.get());
            awaitIdle(registry);
            int emptyVisits = expires.get() - before;
            if (emptyVisits != 1) throw new AssertionError("empty visits=" + emptyVisits);
            clock.addAndGet(TimeUnit.SECONDS.toNanos(5));
            parked();
            if (expires.get() - before != 1) throw new AssertionError("empty visit repeated");
            System.out.printf("empty_index reserved_visits=%d subsequent_visits=0%n", emptyVisits);
        }
    }

    private static void awaitIdle(ZLinkServiceOperationRegistry registry) throws Exception {
        long guard = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (registry.maintenanceTickForTest() != -1) {
            if (System.nanoTime() - guard >= 0)
                throw new AssertionError("maintenance did not become idle");
            Thread.yield();
        }
        parked();
    }

    private static void serialRequestsAcrossRotations() throws Exception {
        worker.set(null);
        AtomicLong clock = new AtomicLong();
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        command -> {
                            Thread thread = new Thread(command, "serial-rotation-visits");
                            worker.set(thread);
                            return thread;
                        },
                        new IllegalStateException("closed"),
                        clock::get)) {
            blocker = registry;
            int reservations = 0;
            int before = expires.get();
            for (int index = 0; index < 1_000; index++) {
                if (registry.maintenanceTickForTest() == -1) reservations++;
                var request =
                        ZLinkCompletionBridge.await(
                                registry.<Integer>register(Duration.ofSeconds(30)));
                parked();
                if (!registry.complete(request.id(), index)) throw new AssertionError("reply lost");
                if (request.get(5, TimeUnit.SECONDS) != index)
                    throw new AssertionError("wrong reply");
                clock.addAndGet(TimeUnit.MILLISECONDS.toNanos(1));
                if (clock.get()
                        >= TimeUnit.MILLISECONDS.toNanos(registry.maintenanceTickForTest())) {
                    java.util.concurrent.locks.LockSupport.unpark(worker.get());
                    awaitIdle(registry);
                }
            }
            int rotations = (1_000 + 255) / 256;
            if (reservations > rotations) throw new AssertionError("reservations=" + reservations);
            System.out.printf(
                    "serial_elapsed_ms=1000 requests=1000 reservations=%d rotations=%d visits=%d%n",
                    reservations, rotations, expires.get() - before);
        }
    }

    private static void queuedVisitDoesNotMultiplyOnRegistrationWakeups() throws Exception {
        worker.set(null);
        AtomicLong clock = new AtomicLong();
        var registry =
                new ZLinkServiceOperationRegistry(
                        command -> {
                            Thread thread = new Thread(command, "queued-deadline-worker");
                            worker.set(thread);
                            return thread;
                        },
                        new IllegalStateException("closed"),
                        clock::get);
        blocker = registry;
        CountDownLatch entered = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        try {
            var request = ZLinkCompletionBridge.await(registry.register(Duration.ofMillis(1)));
            parked();
            var lane = registry.ownerLaneForTest();
            lane.runAsync(
                    () -> {
                        entered.countDown();
                        try {
                            release.await();
                        } catch (InterruptedException failure) {
                            throw new AssertionError(failure);
                        }
                    });
            if (!entered.await(5, TimeUnit.SECONDS))
                throw new AssertionError("owner did not enter");
            clock.set(TimeUnit.MILLISECONDS.toNanos(2));
            for (int wake = 0; wake < 20; wake++) {
                int before = returns.get();
                java.util.concurrent.locks.LockSupport.unpark(worker.get());
                awaitReturns(before + 1);
                parked();
            }
            int queued =
                    systems.zlink.framework.runtime.internal.execution.ZLinkStateLaneTestProbe
                            .queuedTurnCount(lane);
            release.countDown();
            if (queued != 1)
                throw new AssertionError("queued management visits=" + queued + " expected=1");
            try {
                request.get(5, TimeUnit.SECONDS);
                throw new AssertionError("deadline did not expire");
            } catch (ExecutionException failure) {
                if (!(failure.getCause() instanceof TimeoutException)) throw failure;
            }
            System.out.println("queued_wakeups=20 management_turns=1");
        } finally {
            release.countDown();
            registry.close();
        }
    }
}
