package systems.zlink.framework.runtime.internal.service;

import org.junit.jupiter.api.Test;

import java.io.IOException;
import java.lang.classfile.ClassFile;
import java.lang.classfile.ClassTransform;
import java.lang.classfile.instruction.InvokeInstruction;
import java.lang.constant.ClassDesc;
import java.lang.constant.MethodTypeDesc;
import java.lang.management.ManagementFactory;
import java.lang.reflect.InvocationTargetException;
import java.time.Duration;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/** Isolates monitor notifications from real timer scheduling and elapsed wall time. */
public final class ZLinkServiceOperationRegistryWakeTest {
    @Test
    void serialRequestsRetainTheReservedVisitAndIdleDoesNotPoll() throws Throwable {
        // Rewrite only the isolated copy's waits. The real registry's registration,
        // completion, wheel, notification and lifecycle code executes unchanged.
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
                                                                                .equalsString(
                                                                                        "maintainDeadlines"),
                                                                (builder, element) -> {
                                                                    if (element
                                                                                    instanceof
                                                                                    InvokeInstruction
                                                                                            call
                                                                            && call.name()
                                                                                    .equalsString(
                                                                                            "timedWait")) {
                                                                        builder.invokestatic(
                                                                                helper,
                                                                                "timedWait",
                                                                                MethodTypeDesc
                                                                                        .ofDescriptor(
                                                                                                "(Ljava/util/concurrent/TimeUnit;Ljava/lang/Object;J)V"));
                                                                    } else if (element
                                                                                    instanceof
                                                                                    InvokeInstruction
                                                                                            call
                                                                            && call.name()
                                                                                    .equalsString(
                                                                                            "wait")) {
                                                                        builder.invokestatic(
                                                                                helper,
                                                                                "emptyWait",
                                                                                MethodTypeDesc
                                                                                        .ofDescriptor(
                                                                                                "(Ljava/lang/Object;)V"));
                                                                    } else if (element
                                                                                    instanceof
                                                                                    InvokeInstruction
                                                                                            call
                                                                            && call.name()
                                                                                    .equalsString(
                                                                                            "expire")) {
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
    static Object gate;

    public static void timedWait(TimeUnit unit, Object monitor, long remaining)
            throws InterruptedException {
        if (remaining <= 0) throw new AssertionError("nonpositive timed wait");
        monitor.wait(); // The test advances virtual time and notifies at the scheduled visit.
        returns.incrementAndGet();
    }

    public static void emptyWait(Object monitor) throws InterruptedException {
        monitor.wait();
        returns.incrementAndGet();
    }

    public static int expire(ZLinkServiceOperationRegistry registry, long now) {
        expires.incrementAndGet();
        return registry.expire(now);
    }

    static void parked() {
        long guard = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (true) {
            synchronized (gate) {
                var info =
                        ManagementFactory.getThreadMXBean().getThreadInfo(worker.get().threadId());
                if (info != null
                        && info.getThreadState() == Thread.State.WAITING
                        && info.getLockInfo() != null
                        && info.getLockInfo().getIdentityHashCode()
                                == System.identityHashCode(gate)) return;
            }
            if (System.nanoTime() - guard >= 0) throw new AssertionError("worker did not park");
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
        var field = ZLinkServiceOperationRegistry.class.getDeclaredField("gate");
        field.setAccessible(true);
        gate = field.get(registry);
        int serialReturns;
        try (registry) {
            var operation = registry.<Integer>register(Duration.ofMillis(100));
            parked();
            int before = returns.get();
            for (int request = 0; request < 1_000; request++) {
                if (request != 0) operation = registry.register(Duration.ofMillis(100));
                if (!registry.complete(operation.id(), request))
                    throw new AssertionError("reply lost");
                if (operation.get(5, TimeUnit.SECONDS) != request)
                    throw new AssertionError("wrong reply");
                parked();
            }
            serialReturns = returns.get() - before;
            System.out.printf(
                    "serial requests=1000 completed=1000 wait_returns=%d expire_calls=%d upper_bound=2%n",
                    serialReturns, expires.get());
            // Let the already reserved visit occur, then observe indefinite idle.
            synchronized (gate) {
                clock.set(TimeUnit.MILLISECONDS.toNanos(100));
                gate.notifyAll();
            }
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
            var later = registry.register(Duration.ofMillis(200));
            parked();
            if (!registry.discard(later.id())) throw new AssertionError("discard failed");
            ZLinkServiceOperationRegistry.Operation<Object> earlier;
            synchronized (gate) {
                earlier = registry.register(Duration.ofNanos(1));
                clock.addAndGet(TimeUnit.MILLISECONDS.toNanos(2));
            }
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
        System.out.println("PASS");
    }
}
