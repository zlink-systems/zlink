package systems.zlink.framework.runtime.internal.dispatch;

import java.util.ArrayList;
import java.util.ArrayDeque;
import java.util.List;
import java.util.Objects;
import java.util.OptionalLong;
import java.nio.file.Files;
import java.nio.file.Path;
import java.io.IOException;
import java.lang.management.ManagementFactory;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import com.sun.management.OperatingSystemMXBean;
import systems.zlink.framework.configuration.ZLinkApplicationHwmProfile;
import systems.zlink.framework.configuration.ZLinkInboundDispatchOptions;
import systems.zlink.framework.errors.ZLinkConfigurationException;

/**
 * Accounts application payloads that have been received but have not reached
 * a terminal handler result. The receive pause threshold includes both queued
 * payloads and payloads whose handlers have started; a lease releases its
 * reservation only after the handler reaches a terminal result.
 *
 * <p>The budget is shared by every application ingress owned by one Framework
 * runtime. A lease is acquired when a payload enters application dispatch and
 * is closed exactly once when the dispatch reaches its terminal result.</p>
 */
public final class ZLinkInboundDispatchBudget {
    private static final long DEFAULT_MAX_MESSAGE_SIZE = 16_777_216L;
    private static final int DEFAULT_COMPLETION_SEND_LIMIT = 65_536;
    private static final long CGROUP_V1_UNLIMITED_SENTINEL =
        0x7FFF_FFFF_FFFF_0000L;
    private static final Path CGROUP_MEMBERSHIP_FILE =
        Path.of("/proc/self/cgroup");
    private static final List<Path> CGROUP_MEMORY_LIMIT_FILES = List.of(
        Path.of("/sys/fs/cgroup/memory.max"),
        Path.of("/sys/fs/cgroup/memory/memory.limit_in_bytes"));
    private final Object payloadLock = new Object();
    private final Object completionLock = new Object();
    private final long applicationHwmBytes;
    private final int completionSendLimit;
    private final List<Runnable> capacityHandlers = new ArrayList<>();
    private final ArrayDeque<CompletableFuture<CompletionPermit>> completionWaiters =
        new ArrayDeque<>();
    private long pendingPayloadBytes;
    private long activePayloadBytes;
    private long pendingCompletionSends;
    private int activeCompletionSends;
    private volatile boolean completionAdmissionClosed;
    private volatile boolean closed;
    private boolean receivePaused;

    public ZLinkInboundDispatchBudget(long applicationHwmBytes) {
        this(applicationHwmBytes, DEFAULT_COMPLETION_SEND_LIMIT);
    }

    ZLinkInboundDispatchBudget(long applicationHwmBytes, int completionSendLimit) {
        if (applicationHwmBytes < 0) {
            throw new ZLinkConfigurationException(
                "Application HWM bytes must be zero or positive.");
        }
        if (completionSendLimit <= 0) {
            throw new IllegalArgumentException(
                "completionSendLimit must be positive");
        }
        this.applicationHwmBytes = applicationHwmBytes;
        this.completionSendLimit = completionSendLimit;
    }

    public boolean canStartApplicationReceive() {
        synchronized (payloadLock) {
            return !closed && (applicationHwmBytes == 0
                || pendingPayloadBytes < applicationHwmBytes);
        }
    }

    public Lease track(long payloadBytes) {
        if (payloadBytes < 0) {
            throw new IllegalArgumentException("payloadBytes must not be negative");
        }
        synchronized (payloadLock) {
            if (closed) {
                throw new IllegalStateException("inbound dispatch budget is closed");
            }
            pendingPayloadBytes = addExact(
                pendingPayloadBytes, payloadBytes, "pending payload bytes");
            updatePauseUnderLock();
        }
        return new Lease(this, payloadBytes);
    }

    /**
     * Atomically admits one application payload when the host-wide budget has
     * capacity. A message that starts while the budget is below its threshold
     * may complete above that threshold; once the threshold is reached, later
     * application payloads are held until terminal completion releases bytes.
     */
    public Lease tryTrack(long payloadBytes) {
        if (payloadBytes < 0) {
            throw new IllegalArgumentException("payloadBytes must not be negative");
        }
        synchronized (payloadLock) {
            if (closed) {
                return null;
            }
            if (applicationHwmBytes != 0
                && pendingPayloadBytes >= applicationHwmBytes) {
                return null;
            }
            pendingPayloadBytes = addExact(
                pendingPayloadBytes, payloadBytes, "pending payload bytes");
            updatePauseUnderLock();
            return new Lease(this, payloadBytes);
        }
    }

    public AutoCloseable onCapacityAvailable(Runnable handler) {
        Objects.requireNonNull(handler, "handler");
        synchronized (payloadLock) {
            if (closed) {
                return () -> { };
            }
            capacityHandlers.add(handler);
        }
        return () -> {
            synchronized (payloadLock) {
                capacityHandlers.remove(handler);
            }
        };
    }

    public Snapshot snapshot() {
        synchronized (payloadLock) {
            long active = Math.min(activePayloadBytes, pendingPayloadBytes);
            return new Snapshot(
                applicationHwmBytes,
                pendingPayloadBytes,
                pendingPayloadBytes - active,
                active,
                receivePaused);
        }
    }

    /**
     * Acquires the host-wide permit that bounds an application request
     * completion. The returned stage is incomplete while the send limit is
     * occupied; no application thread is blocked while waiting.
     */
    public CompletionStage<CompletionPermit> acquireCompletionPermit() {
        synchronized (completionLock) {
            if (completionAdmissionClosed) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException("inbound completion admission is closed"));
            }
            pendingCompletionSends = addExact(
                pendingCompletionSends, 1, "pending completion sends");
            if (activeCompletionSends < completionSendLimit) {
                activeCompletionSends++;
                return CompletableFuture.completedFuture(new CompletionPermit(this));
            }
            CompletableFuture<CompletionPermit> waiter = new CompletableFuture<>();
            completionWaiters.addLast(waiter);
            waiter.whenComplete((ignored, error) -> removeCancelledCompletionWaiter(waiter));
            return waiter;
        }
    }

    public long pendingCompletionSends() {
        synchronized (completionLock) {
            return pendingCompletionSends;
        }
    }

    public long completionSendLimit() {
        return completionSendLimit;
    }

    /**
     * Stops new payload and completion admission and releases waiters during
     * runtime shutdown. Existing payload leases remain accounted until they
     * reach their terminal result.
     */
    public void close() {
        List<CompletableFuture<CompletionPermit>> waiters;
        synchronized (payloadLock) {
            closed = true;
            receivePaused = true;
            capacityHandlers.clear();
        }
        synchronized (completionLock) {
            completionAdmissionClosed = true;
            waiters = List.copyOf(completionWaiters);
            completionWaiters.clear();
            pendingCompletionSends = 0;
            activeCompletionSends = 0;
        }
        IllegalStateException failure = new IllegalStateException(
            "inbound completion admission is closed");
        waiters.forEach(waiter -> waiter.completeExceptionally(failure));
    }

    private List<Runnable> handlerStartedUnderLock(long payloadBytes) {
        List<Runnable> handlers = List.of();
        boolean wasPaused = receivePaused;
        if (closed) {
            return handlers;
        }
        activePayloadBytes = addExact(
            activePayloadBytes, payloadBytes, "active payload bytes");
        updatePauseUnderLock();
        if (wasPaused && !receivePaused) {
            handlers = List.copyOf(capacityHandlers);
        }
        return handlers;
    }

    private void completed(long payloadBytes, boolean handlerStarted) {
        List<Runnable> handlers = List.of();
        synchronized (payloadLock) {
            if (closed) {
                if (handlerStarted) {
                    activePayloadBytes = Math.max(0, activePayloadBytes - payloadBytes);
                }
                pendingPayloadBytes = Math.max(0, pendingPayloadBytes - payloadBytes);
                return;
            }
            if (handlerStarted) {
                if (payloadBytes > activePayloadBytes) {
                    throw new IllegalStateException(
                        "inbound dispatch active payload accounting underflow");
                }
                activePayloadBytes -= payloadBytes;
            }
            if (payloadBytes > pendingPayloadBytes) {
                throw new IllegalStateException(
                    "inbound dispatch pending payload accounting underflow");
            }
            pendingPayloadBytes -= payloadBytes;
            boolean wasPaused = receivePaused;
            updatePauseUnderLock();
            if (wasPaused && !receivePaused) {
                handlers = List.copyOf(capacityHandlers);
            }
        }
        for (Runnable handler : handlers) {
            try {
                handler.run();
            } catch (RuntimeException ignored) {
                // A wake-up callback must not change accounting or terminal state.
            }
        }
    }

    private void updatePauseUnderLock() {
        receivePaused = applicationHwmBytes != 0
            && pendingPayloadBytes >= applicationHwmBytes;
    }

    private static long addExact(long current, long value, String label) {
        try {
            return Math.addExact(current, value);
        } catch (ArithmeticException overflow) {
            throw new ZLinkConfigurationException(
                label + " exceed the supported long range");
        }
    }

    private void releaseCompletionPermit() {
        CompletableFuture<CompletionPermit> waiter = null;
        synchronized (completionLock) {
            if (completionAdmissionClosed) {
                return;
            }
            if (pendingCompletionSends <= 0 || activeCompletionSends <= 0) {
                throw new IllegalStateException(
                    "inbound completion permit accounting underflow");
            }
            pendingCompletionSends--;
            if (!completionWaiters.isEmpty()) {
                waiter = completionWaiters.removeFirst();
            } else {
                activeCompletionSends--;
            }
        }
        if (waiter != null) {
            CompletionPermit promoted = new CompletionPermit(this);
            if (!waiter.complete(promoted)) {
                promoted.close();
            }
        }
    }

    private void removeCancelledCompletionWaiter(
        CompletableFuture<CompletionPermit> waiter) {
        if (!waiter.isCancelled()) {
            return;
        }
        synchronized (completionLock) {
            if (!completionAdmissionClosed && completionWaiters.remove(waiter)) {
                if (pendingCompletionSends <= 0) {
                    throw new IllegalStateException(
                        "inbound completion permit accounting underflow");
                }
                pendingCompletionSends--;
            }
        }
    }

    public record Snapshot(
        long applicationHwmBytes,
        long pendingPayloadBytes,
        long queuedPayloadBytes,
        long activePayloadBytes,
        boolean applicationReceivePaused) {
        public Snapshot {
            if (applicationHwmBytes < 0
                || pendingPayloadBytes < 0
                || queuedPayloadBytes < 0
                || activePayloadBytes < 0
                || queuedPayloadBytes + activePayloadBytes != pendingPayloadBytes) {
                throw new IllegalArgumentException(
                    "invalid inbound dispatch budget snapshot");
            }
        }
    }

    public static final class Lease implements AutoCloseable {
        private final ZLinkInboundDispatchBudget owner;
        private final long payloadBytes;
        private boolean handlerStarted;
        private boolean closed;

        private Lease(ZLinkInboundDispatchBudget owner, long payloadBytes) {
            this.owner = owner;
            this.payloadBytes = payloadBytes;
        }

        public void handlerStarted() {
            List<Runnable> handlers;
            synchronized (owner.payloadLock) {
                if (closed) {
                    throw new IllegalStateException(
                        "inbound dispatch lease is already closed");
                }
                if (owner.closed) {
                    return;
                }
                if (!handlerStarted) {
                    handlerStarted = true;
                    handlers = owner.handlerStartedUnderLock(payloadBytes);
                } else {
                    handlers = List.of();
                }
            }
            for (Runnable handler : handlers) {
                try {
                    handler.run();
                } catch (RuntimeException ignored) {
                    // A wake-up callback must not change accounting or terminal state.
                }
            }
        }

        @Override
        public void close() {
            synchronized (owner.payloadLock) {
                if (closed) {
                    return;
                }
                closed = true;
            }
            owner.completed(payloadBytes, handlerStarted);
        }
    }

    public static final class CompletionPermit implements AutoCloseable {
        private final ZLinkInboundDispatchBudget owner;
        private boolean closed;

        private CompletionPermit(ZLinkInboundDispatchBudget owner) {
            this.owner = owner;
        }

        @Override
        public void close() {
            synchronized (owner.completionLock) {
                if (closed) {
                    return;
                }
                closed = true;
            }
            owner.releaseCompletionPermit();
        }
    }

    public static long resolveApplicationHwm(ZLinkInboundDispatchOptions options) {
        Objects.requireNonNull(options, "options");
        return resolveApplicationHwm(options, detectedProcessMemoryCandidates());
    }

    /**
     * Resolves Auto HWM from supplied candidates. The package-private seam
     * keeps host-memory detection deterministic in framework tests without
     * exposing an additional application API.
     */
    static long resolveApplicationHwm(
        ZLinkInboundDispatchOptions options,
        ProcessMemoryCandidates candidates) {
        Objects.requireNonNull(options, "options");
        Objects.requireNonNull(candidates, "candidates");
        OptionalLong configured = options.applicationHwmBytes();
        if (configured.isPresent()) {
            long value = configured.getAsLong();
            if (value < 0) {
                throw new ZLinkConfigurationException(
                    "Application HWM bytes must be zero or positive.");
            }
            return value;
        }
        long memoryLimit = options.processMemoryLimitBytes().isPresent()
            ? options.processMemoryLimitBytes().getAsLong()
            : candidates.effectiveLimit();
        if (options.processMemoryLimitBytes().isPresent()
            && memoryLimit <= 0) {
            throw new ZLinkConfigurationException(
                "ProcessMemoryLimitBytes must be a positive finite byte value.");
        }
        if (memoryLimit <= 0) {
            throw new ZLinkConfigurationException(
                "Auto Application HWM requires a positive process memory limit.");
        }
        int percentage = switch (options.applicationHwmProfile()) {
            case COMPACT -> 2;
            case LOW_LATENCY -> 5;
            case BALANCED -> 10;
            case THROUGHPUT -> 20;
        };
        long result = (memoryLimit / 100L) * percentage
            + (memoryLimit % 100L) * percentage / 100L;
        if (result <= 0) {
            throw new ZLinkConfigurationException(
                "Auto Application HWM calculation produced a non-positive value.");
        }
        return result;
    }

    /** Returns the default finite listener limit required by Auto HWM mode. */
    public static long defaultMaxMessageSize() {
        return DEFAULT_MAX_MESSAGE_SIZE;
    }

    private static ProcessMemoryCandidates detectedProcessMemoryCandidates() {
        long cgroupLimit = readCgroupMemoryLimit();
        long managedHeapLimit = Runtime.getRuntime().maxMemory();
        long physicalMemory = 0L;
        try {
            OperatingSystemMXBean operatingSystem =
                ManagementFactory.getPlatformMXBean(OperatingSystemMXBean.class);
            if (operatingSystem != null && operatingSystem.getTotalMemorySize() > 0) {
                physicalMemory = operatingSystem.getTotalMemorySize();
            }
        } catch (RuntimeException ignored) {
            // The JVM may not expose the platform bean in a restricted runtime.
        }
        return new ProcessMemoryCandidates(
            cgroupLimit,
            managedHeapLimit,
            physicalMemory);
    }

    private static long readCgroupMemoryLimit() {
        long smallest = 0L;
        for (Path path : cgroupMemoryLimitFiles()) {
            long limit = readFiniteCgroupLimit(path);
            if (limit > 0 && (smallest == 0 || limit < smallest)) {
                smallest = limit;
            }
        }
        return smallest;
    }

    private static List<Path> cgroupMemoryLimitFiles() {
        List<Path> paths = new ArrayList<>();
        try {
            for (String line : Files.readAllLines(CGROUP_MEMBERSHIP_FILE)) {
                String[] fields = line.split(":", 3);
                if (fields.length != 3) {
                    continue;
                }
                String relative = fields[2].trim();
                while (relative.startsWith("/")) {
                    relative = relative.substring(1);
                }
                if ("0".equals(fields[0])) {
                    paths.add(Path.of("/sys/fs/cgroup")
                        .resolve(relative)
                        .resolve("memory.max"));
                } else if (List.of(fields[1].split(",")).contains("memory")) {
                    paths.add(Path.of("/sys/fs/cgroup/memory")
                        .resolve(relative)
                        .resolve("memory.limit_in_bytes"));
                }
            }
        } catch (IOException | SecurityException ignored) {
            // The root paths below remain available when membership is hidden.
        }
        paths.addAll(CGROUP_MEMORY_LIMIT_FILES);
        return paths;
    }

    private static long readFiniteCgroupLimit(Path path) {
        try {
            String value = Files.readString(path).trim();
            if (value.isEmpty() || value.equals("max")) {
                return 0L;
            }
            long parsed = Long.parseLong(value);
            return parsed > 0 && parsed < CGROUP_V1_UNLIMITED_SENTINEL
                ? parsed
                : 0L;
        } catch (IOException | NumberFormatException | SecurityException ignored) {
            return 0L;
        }
    }

    static record ProcessMemoryCandidates(
        long osLimitBytes,
        long managedHeapLimitBytes,
        long physicalMemoryBytes) {
        long effectiveLimit() {
            long os = finite(osLimitBytes)
                && osLimitBytes < CGROUP_V1_UNLIMITED_SENTINEL
                ? osLimitBytes
                : 0L;
            long heap = finite(managedHeapLimitBytes)
                && managedHeapLimitBytes < CGROUP_V1_UNLIMITED_SENTINEL
                ? managedHeapLimitBytes
                : 0L;
            if (os > 0 && heap > 0) {
                return Math.min(os, heap);
            }
            if (os > 0) {
                return os;
            }
            if (heap > 0) {
                return heap;
            }
            return finite(physicalMemoryBytes) ? physicalMemoryBytes : 0L;
        }

        private static boolean finite(long value) {
            return value > 0 && value < Long.MAX_VALUE;
        }
    }
}
