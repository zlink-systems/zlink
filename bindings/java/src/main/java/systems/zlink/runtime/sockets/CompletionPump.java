/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.contracts.errors.ErrorCategory;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.internal.sockets.SocketOption;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.NativeMessage;

/** Context-owned native wait. Only its thread mutates or waits on the poller. */
public final class CompletionPump implements AutoCloseable {
    private static final int POLLER_EVENT_SIZE = 48;
    private static final long POLLER_EVENT_SOCKET_OFFSET = 8L;
    private static final int RECV_DONT_WAIT = 1;
    private static final AtomicLong NEXT_CONTROL_ENDPOINT = new AtomicLong();
    private final MemorySegment context;
    private final Object lock = new Object();
    private final Map<Long, Registration> registrations = new HashMap<>();
    private final ArrayDeque<Runnable> commands = new ArrayDeque<>();
    private Thread thread;
    private ControlWake wake;
    private MemorySegment poller = MemorySegment.NULL;
    private boolean closed;
    private Throwable failure;

    public CompletionPump(MemorySegment context) {
        this.context = context == null ? MemorySegment.NULL : context;
    }

    void register(CompletionOwner owner) {
        synchronized (lock) {
            if (closed)
                throw new IllegalStateException("completion runtime is closed");
            if (failure != null)
                throwFailure(failure);
            long key = owner.handle().address();
            if (registrations.containsKey(key))
                return;
            if (thread == null) {
                if (context.address() == 0L)
                    throw new IllegalStateException(
                        "automatic completion processing requires a Context-backed socket");
                wake = ControlWake.create(context);
                thread = Thread.ofPlatform().daemon(true)
                    .name("zlink-completion-event-owner").unstarted(this::run);
                try {
                    thread.start();
                } catch (RuntimeException | Error startFailure) {
                    thread = null;
                    wake.close();
                    wake = null;
                    throw startFailure;
                }
            }
            Registration registration = new Registration(owner, key);
            registrations.put(key, registration);
            commands.add(() -> {
                int rc = Native.pollerAdd(poller, MemorySegment.ofAddress(key),
                    MemorySegment.NULL, PollEventFlags.POLLCOMPLETION.mask());
                if (rc != 0)
                    throw ZlinkException.fromLastError(ErrorCategory.CONFIG);
            });
            wake.signal();
        }
    }

    void unregister(CompletionOwner owner) {
        CompletableFuture<Void> removed;
        synchronized (lock) {
            Registration registration = registrations.get(owner.handle().address());
            if (registration == null)
                return;
            if (registration.removed == null) {
                registration.removed = new CompletableFuture<>();
                commands.add(() -> removeRegistration(registration));
                wake.signal();
            }
            removed = registration.removed;
        }
        try {
            removed.join();
        } catch (CompletionException failed) {
            throwFailure(failed.getCause());
        }
    }

    private void removeRegistration(Registration registration) {
        synchronized (lock) {
            if (registrations.get(registration.key) != registration)
                return;
        }
        if (Native.pollerRemove(poller,
                MemorySegment.ofAddress(registration.key)) != 0)
            throw ZlinkException.fromLastError(ErrorCategory.CONFIG);
        CompletableFuture<Void> removed;
        synchronized (lock) {
            registrations.remove(registration.key);
            removed = registration.removed;
        }
        if (removed != null)
            removed.complete(null);
    }

    private void run() {
        Throwable stopped = null;
        try (Arena arena = Arena.ofConfined()) {
            poller = Native.pollerNew();
            if (poller == null || poller.address() == 0L)
                throw ZlinkException.fromLastError(ErrorCategory.CONFIG);
            if (Native.pollerAdd(poller, wake.reader(), MemorySegment.NULL,
                    PollEventFlags.POLLIN.mask()) != 0)
                throw ZlinkException.fromLastError(ErrorCategory.CONFIG);
            MemorySegment events = MemorySegment.NULL;
            int capacity = 0;
            MemorySegment errorOut = arena.allocate(ValueLayout.JAVA_INT);
            while (true) {
                Runnable command;
                int needed;
                synchronized (lock) {
                    if (closed)
                        break;
                    command = commands.pollFirst();
                    needed = registrations.size() + 1;
                }
                if (command != null) {
                    command.run();
                    continue;
                }
                if (capacity < needed) {
                    capacity = Math.max(needed, capacity * 2);
                    events = arena.allocate((long) capacity * POLLER_EVENT_SIZE,
                        ValueLayout.ADDRESS.byteAlignment());
                }
                int count = Native.pollerWait(poller, events, capacity, -1, errorOut);
                if (count < 0)
                    throw ZlinkException.fromLastError(ErrorCategory.CONFIG);
                for (int i = 0; i < count; i++) {
                    long key = events.get(ValueLayout.ADDRESS,
                        (long) i * POLLER_EVENT_SIZE + POLLER_EVENT_SOCKET_OFFSET).address();
                    if (key == wake.reader().address()) {
                        wake.drain();
                        continue;
                    }
                    Registration registration;
                    synchronized (lock) {
                        registration = registrations.get(key);
                    }
                    if (registration != null) {
                        try {
                            registration.owner.drainFromRuntime();
                        } catch (RuntimeException | Error drainFailure) {
                            removeRegistration(registration);
                            registration.owner.rejectRuntimeStates(drainFailure);
                        }
                    }
                }
            }
        } catch (RuntimeException | Error eventFailure) {
            stopped = eventFailure;
        } finally {
            Throwable teardownFailure = null;
            if (poller != null && poller.address() != 0L) {
                try {
                    Native.pollerDestroy(poller);
                } catch (RuntimeException | Error destroyFailure) {
                    teardownFailure = destroyFailure;
                    if (stopped == null)
                        stopped = destroyFailure;
                    else
                        stopped.addSuppressed(destroyFailure);
                }
            }
            ArrayList<Registration> remaining;
            synchronized (lock) {
                failure = stopped;
                remaining = new ArrayList<>(registrations.values());
                registrations.clear();
                commands.clear();
            }
            for (Registration registration : remaining) {
                if (registration.removed != null) {
                    if (teardownFailure == null)
                        registration.removed.complete(null);
                    else
                        registration.removed.completeExceptionally(teardownFailure);
                }
                if (stopped == null)
                    registration.owner.rejectPendingClosed();
                else
                    registration.owner.rejectRuntimeStates(stopped);
            }
        }
    }

    @Override
    public void close() {
        Thread join;
        synchronized (lock) {
            closed = true;
            join = thread;
            if (wake != null)
                wake.signal();
        }
        if (join != null && join != Thread.currentThread()) {
            boolean interrupted = false;
            while (join.isAlive()) {
                try {
                    join.join();
                } catch (InterruptedException ignored) {
                    interrupted = true;
                }
            }
            if (interrupted)
                Thread.currentThread().interrupt();
        }
        synchronized (lock) {
            if (wake != null) {
                wake.close();
                wake = null;
            }
        }
    }

    private static void throwFailure(Throwable cause) {
        if (cause instanceof RuntimeException runtime)
            throw runtime;
        throw (Error) cause;
    }

    private static final class Registration {
        final CompletionOwner owner;
        final long key;
        CompletableFuture<Void> removed;

        Registration(CompletionOwner owner, long key) {
            this.owner = owner;
            this.key = key;
        }
    }
    static final class ControlWake implements AutoCloseable {
        private final MemorySegment reader;
        private final MemorySegment writer;
        private final AtomicBoolean signalled = new AtomicBoolean();
        private final AtomicBoolean closed = new AtomicBoolean();

        private ControlWake(MemorySegment reader, MemorySegment writer) {
            this.reader = reader;
            this.writer = writer;
        }

        static ControlWake create(MemorySegment context) {
            MemorySegment reader = MemorySegment.NULL;
            MemorySegment writer = MemorySegment.NULL;
            try {
                reader = Native.socket(context, SocketType.PAIR.getValue());
                if (reader == null || reader.address() == 0L)
                    throw ZlinkException.fromLastError(ErrorCategory.CONFIG);
                writer = Native.socket(context, SocketType.PAIR.getValue());
                if (writer == null || writer.address() == 0L)
                    throw ZlinkException.fromLastError(ErrorCategory.CONFIG);
                setLingerZero(reader);
                setLingerZero(writer);
                String endpoint = "inproc://zlink-java-completion-control-"
                    + NEXT_CONTROL_ENDPOINT.incrementAndGet();
                try (Arena arena = Arena.ofConfined()) {
                    MemorySegment address = arena.allocateFrom(endpoint,
                        StandardCharsets.UTF_8);
                    if (Native.bind(reader, address) != 0
                            || Native.connect(writer, address) != 0) {
                        throw ZlinkException.fromLastError(
                            ErrorCategory.CONFIG);
                    }
                }
                return new ControlWake(reader, writer);
            } catch (RuntimeException | Error failure) {
                closeHandle(writer);
                closeHandle(reader);
                throw failure;
            }
        }

        MemorySegment reader() {
            return reader;
        }

        void signal() {
            if (closed.get() || !signalled.compareAndSet(false, true))
                return;
            try (Arena arena = Arena.ofConfined()) {
                MemorySegment message = arena.allocate(
                    NativeLayouts.MESSAGE_LAYOUT);
                boolean initialized = false;
                try {
                    if (NativeMessage.messageInitSize(message, 1) != 0) {
                        throw ZlinkException.fromLastError(
                            ErrorCategory.CONFIG);
                    }
                    initialized = true;
                    NativeMessage.messageData(message).reinterpret(1)
                        .set(ValueLayout.JAVA_BYTE, 0, (byte) 1);
                    int result = Native.sendPart(writer, message,
                        SendFlags.NONE.value(), Native.PART_FINAL,
                        MemorySegment.NULL, MemorySegment.NULL);
                    initialized = false;
                    if (result != SubmitResult.OK.value()) {
                        throw new ZlinkSubmitException(
                            SubmitResult.fromValue(result), Native.errno());
                    }
                } finally {
                    if (initialized)
                        NativeMessage.messageClose(message);
                }
            } catch (RuntimeException | Error failure) {
                signalled.set(false);
                throw failure;
            }
        }

        void drain() {
            if (closed.get())
                return;
            try (Arena arena = Arena.ofConfined()) {
                MemorySegment message = arena.allocate(
                    NativeLayouts.MESSAGE_LAYOUT);
                MemorySegment source = arena.allocate(ValueLayout.ADDRESS);
                MemorySegment more = arena.allocate(ValueLayout.JAVA_INT);
                while (true) {
                    if (NativeMessage.messageInit(message) != 0) {
                        throw ZlinkException.fromLastError(
                            ErrorCategory.CONFIG);
                    }
                    int result;
                    int errno;
                    try {
                        result = Native.recv(reader, source, message, more,
                            RECV_DONT_WAIT);
                        errno = result == RecvResult.OK.value()
                            ? 0 : Native.errno();
                    } finally {
                        NativeMessage.messageClose(message);
                    }
                    if (result == RecvResult.NO_DATA.value()) {
                        signalled.set(false);
                        return;
                    }
                    if (result != RecvResult.OK.value()) {
                        throw new ZlinkRecvException(
                            RecvResult.fromValue(result), errno);
                    }
                }
            }
        }

        @Override
        public void close() {
            if (!closed.compareAndSet(false, true))
                return;
            closeHandle(writer);
            closeHandle(reader);
        }

        private static void setLingerZero(MemorySegment handle) {
            try (Arena arena = Arena.ofConfined()) {
                MemorySegment zero = arena.allocate(ValueLayout.JAVA_INT);
                zero.set(ValueLayout.JAVA_INT, 0, 0);
                int optionId = SocketOptionRouter.route(
                    SocketOption.LINGER.getValue(), SocketType.PAIR)
                    .nativeCommonOptionId();
                if (Native.setSockOpt(handle, optionId, zero,
                        ValueLayout.JAVA_INT.byteSize()) != 0) {
                    throw ZlinkException.fromLastError(ErrorCategory.CONFIG);
                }
            }
        }

        private static void closeHandle(MemorySegment handle) {
            if (handle != null && handle.address() != 0L)
                Native.close(handle);
        }
    }

}
