/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.core;

import systems.zlink.contracts.core.ZlinkThread;

import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.Objects;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

final class NativeZlinkThread implements ZlinkThread {
    private static final Linker LINKER = Linker.nativeLinker();
    private static final FunctionDescriptor THREAD_ENTRY =
      FunctionDescriptor.ofVoid(ValueLayout.ADDRESS);
    private static final Arena CALLBACK_ARENA = Arena.ofShared();
    private static final MethodHandle ENTRY_HANDLE = callbackHandle();
    private static final MemorySegment ENTRY_STUB = LINKER.upcallStub(
      ENTRY_HANDLE, THREAD_ENTRY, CALLBACK_ARENA);
    private static final AtomicLong NEXT_TOKEN = new AtomicLong(1L);
    private static final ConcurrentHashMap<Long, NativeZlinkThread> THREADS =
      new ConcurrentHashMap<>();

    private final Runnable task;
    private final long token;
    private MemorySegment handle;
    private volatile Throwable failure;
    private volatile boolean joined;

    NativeZlinkThread(Runnable task) {
        this.task = Objects.requireNonNull(task, "task");
        this.token = NEXT_TOKEN.getAndIncrement();
        THREADS.put(token, this);
        this.handle = Native.threadStart(ENTRY_STUB, MemorySegment.ofAddress(token));
        if (handle == null || handle.address() == 0) {
            THREADS.remove(token);
            throw new ZlinkConfigException(ConfigResult.INVALID_HANDLE);
        }
    }

    public void join() {
        ensureOpen();
        Native.threadJoin(handle);
        joined = true;
        THREADS.remove(token, this);
        handle = MemorySegment.NULL;
        if (failure != null) {
            if (failure instanceof RuntimeException re) {
                throw re;
            }
            throw new RuntimeException("zlink thread task failed", failure);
        }
    }

    @Override
    public void close() {
        if (handle == null || handle.address() == 0) {
            return;
        }
        join();
    }

    private static MethodHandle callbackHandle() {
        try {
            return MethodHandles.lookup().findStatic(NativeZlinkThread.class,
                "handleEntry", MethodType.methodType(void.class,
                    MemorySegment.class));
        } catch (ReflectiveOperationException ex) {
            throw new ExceptionInInitializerError(ex);
        }
    }

    private static void handleEntry(MemorySegment arg) {
        long token = arg.address();
        NativeZlinkThread thread = THREADS.get(token);
        if (thread == null) {
            return;
        }
        InternalAccess.enterCallback();
        try {
            thread.task.run();
        } catch (Throwable failure) {
            thread.failure = failure;
        } finally {
            InternalAccess.leaveCallback();
        }
    }

    private void ensureOpen() {
        if (handle == null || handle.address() == 0) {
            throw new IllegalStateException("thread is closed");
        }
    }
}
