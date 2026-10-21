package systems.zlink.bench.withgrpc.client;

import systems.zlink.bench.withgrpc.shared.*;
import systems.zlink.contracts.core.*;
import systems.zlink.contracts.messaging.*;
import systems.zlink.contracts.sockets.*;

import java.lang.management.ManagementFactory;
import java.net.ServerSocket;
import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Public API requests. No application limit on unfinished replies. */
public final class RequestBackpressureProbe {
    public static final Duration TIMEOUT = Duration.ofSeconds(30);
    static final Duration POLL = Duration.ofMillis(10);
    public static final int PAYLOAD_BYTES = 1024, RUN_ID = 42, COST_REQUESTS = 1000;
    static final CompletableFuture<Void> TICK = CompletableFuture.completedFuture(null);
    static final long SAMPLE_NS = 100_000_000L, STOP = Long.MAX_VALUE;
    static final com.sun.management.ThreadMXBean THREADS =
            (com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean();
    static final com.sun.management.OperatingSystemMXBean PROCESS =
            (com.sun.management.OperatingSystemMXBean) ManagementFactory.getOperatingSystemMXBean();
    static String costDirectory;

    public interface Arm extends AutoCloseable {
        BenchRequestOperation.Submission submit(int bytes, byte phase, long sequence);

        boolean await(CompletableFuture<Void> future, Duration duration) throws Exception;

        String name();

        void tick() throws Exception;

        default String pressure() {
            return "target_pressure=not_applicable";
        }
    }

    static String endpoint() throws Exception {
        try (var s = new ServerSocket(0)) {
            return "tcp://127.0.0.1:" + s.getLocalPort();
        }
    }

    static final class BindingArm implements Arm {
        final Context context = Zlink.createContext();
        final RouterSocket server = context.createRouterSocket();
        final RawStack raw;
        final RawStack.RawOperation operation;
        final CompletableFuture<Void> ownerResult = new CompletableFuture<>();
        final Thread owner;

        BindingArm() throws Exception {
            var address = endpoint();
            server.setRoutingId(RoutingId.from(RawWire.RAW_REQUEST_SERVER_ID));
            server.options().mandatory(true);
            server.bind(address);
            owner =
                    Thread.ofPlatform()
                            .name("probe-binding-server")
                            .start(
                                    () -> {
                                        try (var received = new Received()) {
                                            while (true) {
                                                try {
                                                    if (!server.recv(received, RecvFlags.NONE))
                                                        continue;
                                                } catch (
                                                        systems.zlink.contracts.errors
                                                                        .ZlinkRecvException
                                                                e) {
                                                    if (e.getResult() == RecvResult.NO_DATA)
                                                        continue;
                                                    throw e;
                                                }
                                                var body =
                                                        RawWire.decodeBenchPayloadBody(
                                                                received.parts()
                                                                        .getLast()
                                                                        .dataBuffer());
                                                long seq =
                                                        BenchMetricHeader.decode(body).sequence();
                                                try (var header =
                                                                Message.from(
                                                                        RawWire.RESPONSE_ENVELOPE);
                                                        var reply =
                                                                RawWire.encodeBenchPayloadMessage(
                                                                        body)) {
                                                    received.reply()
                                                            .message(header)
                                                            .message(reply)
                                                            .submit();
                                                }
                                                if (seq == STOP) break;
                                            }
                                            ownerResult.complete(null);
                                        } catch (Throwable e) {
                                            ownerResult.completeExceptionally(e);
                                        }
                                    });
            var options =
                    new BenchOptions(
                            new String[] {
                                "--scenario",
                                "request-backpressure",
                                "--run-id",
                                "j-bp-public",
                                "--cell-id",
                                "bp-" + PAYLOAD_BYTES,
                                "--trigger-url",
                                "http://127.0.0.1:1",
                                "--stats-url",
                                "http://127.0.0.1:2",
                                "--target-stats-url",
                                "http://127.0.0.1:3",
                                "--target-endpoint",
                                address
                            });
            raw =
                    RawStack.create(
                            context,
                            options,
                            "probe-binding-client",
                            RawWire.RAW_REQUEST_SERVER_ID,
                            address);
            operation = raw.request();
            new BenchDrivers(options).waitForRouteReady(operation, PAYLOAD_BYTES);
        }

        public String name() {
            return "binding";
        }

        public void tick() throws Exception {
            operation.await(TICK, Duration.ZERO);
        }

        public BenchRequestOperation.Submission submit(int bytes, byte phase, long sequence) {
            var s = operation.submitRaw(bytes, phase, sequence);
            if (s.result() != SubmitResult.OK && s.result() != SubmitResult.BACKPRESSURED)
                throw new AssertionError("binding submit " + s.result());
            return new BenchRequestOperation.Submission(s.admitted(), s.completion());
        }

        public boolean await(CompletableFuture<Void> f, Duration d) throws Exception {
            return operation.await(f, d);
        }

        public void close() throws Exception {
            try {
                var s = operation.submitRaw(PAYLOAD_BYTES, (byte) 0, STOP);
                if (!operation.await(s.completion(), TIMEOUT))
                    throw new AssertionError("binding release timeout");
                ownerResult.get(TIMEOUT.toNanos(), TimeUnit.NANOSECONDS);
                owner.join();
            } finally {
                raw.close();
                server.close();
                context.close();
            }
        }
    }

    static final class Metrics {
        final AtomicLong completed = new AtomicLong(),
                inflight = new AtomicLong(),
                maximum = new AtomicLong();
        final AtomicReference<Throwable> error = new AtomicReference<>();
        final CompletableFuture<Void> drained = new CompletableFuture<>();
        volatile boolean sealed;
        long submitted, pauses, resumes, pauseNs;
        long sequence;

        void begin() {
            submitted++;
            long n = inflight.incrementAndGet();
            maximum.accumulateAndGet(n, Math::max);
        }

        void complete(Throwable failure) {
            if (failure != null) error.compareAndSet(null, failure);
            completed.incrementAndGet();
            if (inflight.decrementAndGet() == 0 && sealed) drained.complete(null);
        }

        void seal() {
            sealed = true;
            if (inflight.get() == 0) drained.complete(null);
        }
    }

    record Sample(
            long ns,
            long submitted,
            long completed,
            long inflight,
            long pauses,
            long resumes,
            String pressure) {}

    record ThreadCost(long cpu, long allocated) {}

    static Map<Long, ThreadCost> costs() {
        var out = new HashMap<Long, ThreadCost>();
        for (long id : THREADS.getAllThreadIds()) {
            long c = THREADS.getThreadCpuTime(id), a = THREADS.getThreadAllocatedBytes(id);
            if (c >= 0 && a >= 0) out.put(id, new ThreadCost(c, a));
        }
        return out;
    }

    static void window(Arm arm, int window, long durationNs, boolean active) throws Exception {
        jdk.jfr.Recording recording = null;
        if (active && costDirectory != null) {
            recording = new jdk.jfr.Recording();
            recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
            recording.enable("jdk.DataLoss");
            recording.enable("jdk.VirtualThreadStart").withStackTrace();
            recording.start();
        }
        var m = new Metrics();
        var samples = new ArrayList<Sample>();
        long start = System.nanoTime(), deadline = start + durationNs, next = start + SAMPLE_NS;
        long processStart = PROCESS.getProcessCpuTime(),
                callerStart = THREADS.getCurrentThreadCpuTime();
        var threadStart = costs();
        CompletableFuture<Void> admission = null;
        long pauseStart = 0;
        while (active && costDirectory != null
                ? m.submitted < COST_REQUESTS
                : System.nanoTime() < deadline) {
            if (admission != null) {
                if (arm.await(admission, POLL)) {
                    m.resumes++;
                    m.pauseNs += System.nanoTime() - pauseStart;
                    admission = null;
                }
            } else {
                m.begin();
                BenchRequestOperation.Submission s;
                try {
                    s =
                            arm.submit(
                                    PAYLOAD_BYTES,
                                    active
                                            ? BenchMetricHeader.PHASE_ACTIVE
                                            : BenchMetricHeader.PHASE_WARMUP,
                                    m.sequence++);
                } catch (Throwable t) {
                    m.complete(t);
                    throw t;
                }
                s.reply().whenComplete((v, e) -> m.complete(e));
                if (!s.admitted().isDone()) {
                    m.pauses++;
                    pauseStart = System.nanoTime();
                    admission = s.admitted();
                } else s.admitted().join();
                arm.tick(); // Binding owns explicit completion progress; Framework owns its pump.
            }
            long now = System.nanoTime();
            if (now >= next) {
                samples.add(
                        new Sample(
                                now - start,
                                m.submitted,
                                m.completed.get(),
                                m.inflight.get(),
                                m.pauses,
                                m.resumes,
                                arm.pressure()));
                next = now + SAMPLE_NS;
            }
            if (m.error.get() != null) throw new AssertionError("request failed", m.error.get());
        }
        long stopped = System.nanoTime(),
                caller = THREADS.getCurrentThreadCpuTime() - callerStart,
                process = PROCESS.getProcessCpuTime() - processStart;
        long threadCpu = 0, allocation = 0;
        var threadEnd = costs();
        for (var entry : threadEnd.entrySet()) {
            var before = threadStart.get(entry.getKey());
            if (before != null) {
                threadCpu += entry.getValue().cpu() - before.cpu();
                allocation += entry.getValue().allocated() - before.allocated();
            }
        }
        long completedAtStop = m.completed.get(),
                inflightAtStop = m.inflight.get(),
                resumesAtStop = m.resumes,
                pauseAtStop = m.pauseNs + (admission == null ? 0 : stopped - pauseStart);
        m.seal();
        if (admission != null) {
            if (!arm.await(admission, TIMEOUT)) throw new AssertionError("admission drain timeout");
            m.resumes++;
            m.pauseNs += System.nanoTime() - pauseStart;
        }
        if (!arm.await(m.drained, TIMEOUT)) throw new AssertionError("reply drain timeout");
        if (m.error.get() != null) throw new AssertionError("request failed", m.error.get());
        if (m.submitted != m.completed.get()) throw new AssertionError("reply conservation");
        long fullNs = System.nanoTime() - start,
                fullProcess = PROCESS.getProcessCpuTime() - processStart,
                fullCaller = THREADS.getCurrentThreadCpuTime() - callerStart;
        if (recording != null) {
            recording.stop();
            recording.dump(
                    java.nio.file.Path.of(costDirectory, arm.name() + "-" + window + ".jfr"));
            recording.close();
        }
        if (active) {
            for (var s : samples)
                System.out.printf(
                        Locale.ROOT,
                        "SERIES arm=%s window=%d ns=%d submitted=%d completed=%d inflight=%d"
                                + " admission_stalls=%d admission_resumes=%d %s%n",
                        arm.name(),
                        window,
                        s.ns(),
                        s.submitted(),
                        s.completed(),
                        s.inflight(),
                        s.pauses(),
                        s.resumes(),
                        s.pressure());
            System.out.printf(
                    Locale.ROOT,
                    "WINDOW arm=%s window=%d ns=%d submitted=%d completed=%d drained=%d inflight=%d"
                            + " max_inflight=%d admission_stalls=%d admission_resumes=%d"
                            + " admission_stall_ns=%d caller_cpu_ns=%d process_cpu_ns=%d"
                            + " live_platform_cpu_ns=%d live_platform_alloc_bytes=%d full_ns=%d"
                            + " full_process_cpu_ns=%d full_caller_cpu_ns=%d end_inflight=%d%n",
                    arm.name(),
                    window,
                    stopped - start,
                    m.submitted,
                    completedAtStop,
                    m.completed.get(),
                    inflightAtStop,
                    m.maximum.get(),
                    m.pauses,
                    resumesAtStop,
                    pauseAtStop,
                    caller,
                    process,
                    threadCpu,
                    allocation,
                    fullNs,
                    fullProcess,
                    fullCaller,
                    m.inflight.get());
        }
    }

    static void configureCost(String directory) {
        costDirectory = directory;
        if (costDirectory != null) {
            if (!ManagementFactory.getRuntimeMXBean().getInputArguments().contains("-XX:-UseTLAB"))
                throw new IllegalArgumentException(
                        "Full allocation recording requires -XX:-UseTLAB");
            System.out.println(
                    "META cost_mode=true fixed_requests="
                            + COST_REQUESTS
                            + " jfr_includes_drain=true"
                            + " jfr_includes_harness=true timing_comparison=false");
        }
    }

    public static void main(String[] args) throws Exception {
        int rounds = args.length > 0 ? Integer.parseInt(args[0]) : 2;
        long nanos = args.length > 1 ? Long.parseLong(args[1]) * 1_000_000 : 1000_000_000;
        configureCost(args.length > 2 ? args[2] : null);
        THREADS.setThreadCpuTimeEnabled(true);
        THREADS.setThreadAllocatedMemoryEnabled(true);
        System.out.println(
                "META public_api=true payload="
                        + PAYLOAD_BYTES
                        + " inflight_cap=none"
                        + " allocation_coverage=surviving_platform_threads_only"
                        + " virtual_thread_allocation=excluded process_cpu=all_threads"
                        + " drain_excluded_from_window=true");
        System.out.println(
                "META available_processors="
                        + Runtime.getRuntime().availableProcessors()
                        + " max_heap="
                        + Runtime.getRuntime().maxMemory());
        try (var binding = new BindingArm();
                var framework = new RequestBackpressureFramework("framework")) {
            window(binding, -1, nanos, false);
            window(framework, -1, nanos, false);
            for (int round = 0; round < rounds; round++) {
                Arm[] order =
                        round % 2 == 0
                                ? new Arm[] {binding, framework}
                                : new Arm[] {framework, binding};
                for (Arm arm : order) window(arm, round, nanos, true);
            }
        }
    }
}
