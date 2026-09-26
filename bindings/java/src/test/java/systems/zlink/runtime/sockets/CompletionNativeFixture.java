/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static java.lang.foreign.ValueLayout.*;
import static org.junit.jupiter.api.Assertions.*;

import java.lang.foreign.*;
import java.lang.invoke.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.*;
import java.util.concurrent.*;
import systems.zlink.TestSupport;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.sockets.*;
import systems.zlink.runtime.nativeapi.*;

/** Installs scripted Core ABI outcomes only in a disposable probe JVM. */
final class CompletionNativeFixture {
    record Attempt(SubmitResult result, int errno, long id, boolean block) {
        Attempt(SubmitResult result, int errno, long id) {
            this(result, errno, id, false);
        }
    }

    record Submission(long context, long id, boolean request, byte[] rid) {}
    record Record(int kind, Submission submission, int result, int errno) {}

    final Queue<Attempt> attempts = new ConcurrentLinkedQueue<>();
    final Queue<Record> completions = new ConcurrentLinkedQueue<>();
    final List<Submission> submissions = new CopyOnWriteArrayList<>();
    final List<String> order = new CopyOnWriteArrayList<>();
    final CountDownLatch admissionEntered = new CountDownLatch(1);
    final CountDownLatch releaseAdmission = new CountDownLatch(1);
    boolean omitRidEcho;
    final java.util.concurrent.atomic.AtomicInteger pollerDestroyBusy =
        new java.util.concurrent.atomic.AtomicInteger();
    final java.util.concurrent.atomic.AtomicInteger pollerWaitCalls =
        new java.util.concurrent.atomic.AtomicInteger();
    final CountDownLatch pollerWaitEntered = new CountDownLatch(1);
    final CountDownLatch releasePollerWait = new CountDownLatch(1);
    volatile boolean blockPollerWait;
    final java.util.concurrent.atomic.AtomicInteger ctxTermCalls =
        new java.util.concurrent.atomic.AtomicInteger();
    final java.util.concurrent.atomic.AtomicInteger ctxTermInterrupted =
        new java.util.concurrent.atomic.AtomicInteger();
    private MethodHandle corePollerDestroy;
    private MethodHandle corePollerWait;
    private MethodHandle coreCtxTerm;
    private MethodHandle coreSend;
    /** Sends reach Core instead of the scripted attempts. */
    volatile boolean passthroughSends;
    private final Map<String, MethodHandle> coreReceives = new HashMap<>();
    private final Map<String, Queue<int[]>> receiveFaults = new ConcurrentHashMap<>();
    volatile boolean unexpectedSubmit;
    volatile boolean admissionInterrupted;
    private final Arena arena = Arena.ofShared();
    private final MethodHandle errnoLocation;
    private int closedRecords;

    CompletionNativeFixture() throws Throwable {
        // Replace the loader's existing lookup before Native initializes. All
        // unlisted symbols still resolve to the selected local Core library.
        Class<?> loader = Class.forName("systems.zlink.runtime.nativeapi.LibraryLoader");
        var lookupMethod = loader.getDeclaredMethod("lookup");
        lookupMethod.setAccessible(true);
        SymbolLookup core = (SymbolLookup) lookupMethod.invoke(null);
        Linker linker = Linker.nativeLinker();
        errnoLocation = linker.downcallHandle(linker.defaultLookup()
            .find("__errno_location").orElseThrow(), FunctionDescriptor.of(ADDRESS));
        Map<String, MemorySegment> replacements = new HashMap<>();
        coreSend = linker.downcallHandle(core.find("zlink_send").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG, JAVA_INT, ADDRESS,
                ADDRESS));
        replace(replacements, "zlink_send", "send", FunctionDescriptor.of(JAVA_INT,
            ADDRESS, ADDRESS, JAVA_LONG, JAVA_INT, ADDRESS, ADDRESS));
        replace(replacements, "zlink_send_rid", "sendRid", FunctionDescriptor.of(JAVA_INT,
            ADDRESS, ADDRESS, ADDRESS, JAVA_LONG, JAVA_INT, ADDRESS, ADDRESS));
        replace(replacements, "zlink_request", "request", FunctionDescriptor.of(JAVA_INT,
            ADDRESS, ADDRESS, ADDRESS, JAVA_LONG, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS));
        replace(replacements, "zlink_reply", "reply", FunctionDescriptor.of(JAVA_INT,
            ADDRESS, ADDRESS, JAVA_LONG, ADDRESS, JAVA_LONG));
        replace(replacements, "zlink_disconnect_rid", "disconnect", FunctionDescriptor.of(JAVA_INT,
            ADDRESS, ADDRESS));
        replace(replacements, "zlink_completion_recv", "recv", FunctionDescriptor.of(JAVA_INT,
            ADDRESS, ADDRESS, JAVA_INT));
        replace(replacements, "zlink_completion_close", "closeRecord", FunctionDescriptor.ofVoid(ADDRESS));
        FunctionDescriptor handleClose = FunctionDescriptor.of(JAVA_INT, ADDRESS);
        corePollerDestroy = linker.downcallHandle(
            core.find("zlink_poller_destroy").orElseThrow(), handleClose);
        replace(replacements, "zlink_poller_destroy", "pollerDestroy", handleClose);
        FunctionDescriptor pollerWaitDescriptor = FunctionDescriptor.of(
            JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, JAVA_LONG, ADDRESS);
        corePollerWait = linker.downcallHandle(
            core.find("zlink_poller_wait").orElseThrow(), pollerWaitDescriptor);
        replace(replacements, "zlink_poller_wait", "pollerWait",
            pollerWaitDescriptor);
        coreCtxTerm = linker.downcallHandle(core.find("zlink_ctx_term").orElseThrow(),
            handleClose);
        replace(replacements, "zlink_ctx_term", "ctxTerm", handleClose);
        receive(core, linker, replacements, "zlink_recv", "recvData", FunctionDescriptor.of(
            JAVA_INT, ADDRESS, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS, JAVA_INT));
        receive(core, linker, replacements, "zlink_router_recv", "routerRecv",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, ADDRESS, ADDRESS, JAVA_LONG,
                ADDRESS, JAVA_INT));
        receive(core, linker, replacements, "zlink_subscribe", "subscribeRecv",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS,
                ADDRESS, JAVA_LONG, ADDRESS, JAVA_INT));
        receive(core, linker, replacements, "zlink_xpub_recv", "xpubRecv",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, ADDRESS, ADDRESS, JAVA_LONG,
                ADDRESS, JAVA_INT));
        var lookupField = loader.getDeclaredField("LOOKUP");
        lookupField.setAccessible(true);
        lookupField.set(null, (SymbolLookup) name -> {
            MemorySegment symbol = replacements.get(name);
            return symbol == null ? core.find(name) : Optional.of(symbol);
        });
    }

    private void replace(Map<String, MemorySegment> replacements, String symbol,
                         String method, FunctionDescriptor descriptor) throws ReflectiveOperationException {
        MethodHandle handle = MethodHandles.lookup().findVirtual(getClass(), method,
            descriptor.toMethodType()).bindTo(this);
        replacements.put(symbol, Linker.nativeLinker().upcallStub(handle, descriptor, arena));
    }

    // Receive symbols are scripted only for blocking calls: the DONT_WAIT
    // critical downcalls of the same symbols must not reach an upcall.
    private void receive(SymbolLookup core, Linker linker,
                         Map<String, MemorySegment> replacements, String symbol,
                         String method, FunctionDescriptor descriptor)
            throws ReflectiveOperationException {
        coreReceives.put(symbol, linker.downcallHandle(core.find(symbol).orElseThrow(),
            descriptor));
        replace(replacements, symbol, method, descriptor);
    }

    /** The next call of {@code symbol} returns {@code result} with {@code errno}. */
    void receiveFault(String symbol, RecvResult result, int errno) {
        receiveFaults.computeIfAbsent(symbol, ignored -> new ConcurrentLinkedQueue<>())
            .add(new int[] {result.value(), errno});
    }

    private int scriptedReceive(String symbol, Object... args) throws Throwable {
        Queue<int[]> faults = receiveFaults.get(symbol);
        int[] fault = faults == null ? null : faults.poll();
        if (fault != null) {
            errno(fault[1]);
            return fault[0];
        }
        return (int) coreReceives.get(symbol).invokeWithArguments(args);
    }

    private int recvData(MemorySegment s, MemorySegment rid, MemorySegment parts,
                         long capacity, MemorySegment count, int flags) throws Throwable {
        return scriptedReceive("zlink_recv", s, rid, parts, capacity, count, flags);
    }

    private int routerRecv(MemorySegment s, MemorySegment rid, MemorySegment token,
                           MemorySegment parts, long capacity, MemorySegment count,
                           int flags) throws Throwable {
        return scriptedReceive("zlink_router_recv", s, rid, token, parts, capacity, count,
            flags);
    }

    private int subscribeRecv(MemorySegment s, MemorySegment rid, MemorySegment topic,
                              long topicCapacity, MemorySegment topicLength,
                              MemorySegment parts, long capacity, MemorySegment count,
                              int flags) throws Throwable {
        return scriptedReceive("zlink_subscribe", s, rid, topic, topicCapacity, topicLength,
            parts, capacity, count, flags);
    }

    private int xpubRecv(MemorySegment s, MemorySegment rid, MemorySegment subscribed,
                         MemorySegment topic, long topicCapacity, MemorySegment topicLength,
                         int flags) throws Throwable {
        return scriptedReceive("zlink_xpub_recv", s, rid, subscribed, topic, topicCapacity,
            topicLength, flags);
    }

    private void errno(int value) throws Throwable {
        ((MemorySegment) errnoLocation.invokeExact()).reinterpret(JAVA_INT.byteSize())
            .set(JAVA_INT, 0, value);
    }

    private int submit(MemorySegment parts, long partCount, MemorySegment context,
                       MemorySegment idOut, boolean request, MemorySegment rid) throws Throwable {
        long partSize = NativeLayouts.MESSAGE_LAYOUT.byteSize();
        parts = parts.reinterpret(Math.multiplyExact(partSize, partCount));
        for (long index = 0; index < partCount; index++) {
            NativeMessage.messageClose(parts.asSlice(index * partSize,
                partSize));
        }
        Attempt attempt = attempts.poll();
        if (attempt == null) {
            unexpectedSubmit = true;
            errno(NativeErrno.EINVAL);
            return SubmitResult.INTERNAL_ERROR.value();
        }
        order.add("submit:" + attempt.id());
        submissions.add(new Submission(context.address(), attempt.id(), request,
            rid.equals(MemorySegment.NULL) ? null : rid.reinterpret(
                NativeLayouts.ROUTING_ID_LAYOUT.byteSize()).toArray(JAVA_BYTE)));
        if (attempt.block()) {
            admissionEntered.countDown();
            try {
                releaseAdmission.await();
            } catch (InterruptedException interrupted) {
                admissionInterrupted = true;
                Thread.currentThread().interrupt();
            }
        }
        if (!idOut.equals(MemorySegment.NULL))
            idOut.reinterpret(JAVA_LONG.byteSize()).set(JAVA_LONG, 0, attempt.id());
        errno(attempt.errno());
        return attempt.result().value();
    }

    private int send(MemorySegment socket, MemorySegment parts, long partCount, int flags,
                     MemorySegment context, MemorySegment idOut) throws Throwable {
        if (passthroughSends)
            return (int) coreSend.invokeExact(socket, parts, partCount, flags, context, idOut);
        return submit(parts, partCount, context, idOut, false,
            MemorySegment.NULL);
    }

    private int sendRid(MemorySegment socket, MemorySegment rid, MemorySegment parts,
                        long partCount, int flags, MemorySegment context,
                        MemorySegment idOut) throws Throwable {
        return submit(parts, partCount, context, idOut, false, rid);
    }

    private int request(MemorySegment socket, MemorySegment rid, MemorySegment parts,
                        long partCount, int flags, int timeout, MemorySegment context,
                        MemorySegment idOut) throws Throwable {
        return submit(parts, partCount, context, idOut, true, rid);
    }

    private int reply(MemorySegment socket, MemorySegment rid, long token,
                      MemorySegment parts, long partCount) throws Throwable {
        return submit(parts, partCount, MemorySegment.NULL,
            MemorySegment.NULL, false, rid);
    }

    private int disconnect(MemorySegment socket, MemorySegment rid) {
        order.add("disconnect");
        return 0;
    }

    private int recv(MemorySegment socket, MemorySegment output, int flags) throws Throwable {
        Record record = completions.poll();
        if (record == null) {
            order.add("recv:NO_DATA");
            errno(NativeErrno.EAGAIN);
            return RecvResult.NO_DATA.value();
        }
        order.add("recv:" + record.submission().id());
        MemorySegment completion = output.reinterpret(NativeLayouts.COMPLETION_LAYOUT.byteSize());
        completion.set(JAVA_INT, NativeLayouts.COMPLETION_KIND_OFFSET, record.kind());
        completion.set(JAVA_LONG, NativeLayouts.COMPLETION_ID_OFFSET, record.submission().id());
        completion.set(ADDRESS, NativeLayouts.COMPLETION_CONTEXT_OFFSET,
            MemorySegment.ofAddress(record.submission().context()));
        if (record.kind() == CompletionKind.REQUEST.value()) {
            completion.set(JAVA_INT, NativeLayouts.COMPLETION_REQUEST_RESULT_OFFSET, record.result());
        } else {
            completion.set(JAVA_INT, NativeLayouts.COMPLETION_SEND_RESULT_OFFSET, record.result());
            completion.set(JAVA_INT, NativeLayouts.COMPLETION_SEND_ERRNO_OFFSET, record.errno());
        }
        if (!omitRidEcho && record.submission().rid() != null)
            MemorySegment.copy(MemorySegment.ofArray(record.submission().rid()), 0,
                completion, NativeLayouts.COMPLETION_PEER_RID_OFFSET,
                NativeLayouts.ROUTING_ID_LAYOUT.byteSize());
        return RecvResult.OK.value();
    }

    private int pollerDestroy(MemorySegment holder) throws Throwable {
        if (pollerDestroyBusy.getAndUpdate(count -> Math.max(0, count - 1)) > 0) {
            errno(NativeErrno.EBUSY);
            return CloseResult.BUSY.value();
        }
        return (int) corePollerDestroy.invokeExact(holder);
    }

    private int pollerWait(MemorySegment poller, MemorySegment events,
                           int count, long timeout, MemorySegment errorOut)
            throws Throwable {
        if (!blockPollerWait)
            return (int) corePollerWait.invokeExact(poller, events, count,
                timeout, errorOut);
        if (pollerWaitCalls.incrementAndGet() == 1) {
            pollerWaitEntered.countDown();
            if (!releasePollerWait.await(TestSupport.DEFAULT_TIMEOUT_MS,
                    TimeUnit.MILLISECONDS))
                throw new AssertionError("poller wait was not released");
            return 0;
        }
        errorOut.reinterpret(JAVA_INT.byteSize())
            .set(JAVA_INT, 0, ConfigResult.BUSY.value());
        errno(NativeErrno.EBUSY);
        return -1;
    }

    private int ctxTerm(MemorySegment ctx) throws Throwable {
        ctxTermCalls.incrementAndGet();
        if (ctxTermInterrupted.getAndUpdate(count -> Math.max(0, count - 1)) > 0) {
            errno(NativeErrno.EINTR);
            return CloseResult.INTERNAL_ERROR.value();
        }
        return (int) coreCtxTerm.invokeExact(ctx);
    }

    private void closeRecord(MemorySegment completion) {
        closedRecords++;
    }

    void writable(Submission submission, int errno) {
        completions.add(new Record(CompletionKind.WRITABLE.value(), submission,
            errno == 0 ? 0 : 202, errno));
    }

    void requestResult(Submission submission, RequestResult result) {
        completions.add(new Record(CompletionKind.REQUEST.value(), submission, result.value(), 0));
    }

    void verify(int expectedClosedRecords) {
        assertFalse(unexpectedSubmit, "every native submission must have a scripted outcome");
        assertFalse(admissionInterrupted);
        assertTrue(attempts.isEmpty(), "all scripted submissions must run");
        assertTrue(completions.isEmpty(), "all queued completions must be drained");
        assertEquals(expectedClosedRecords, closedRecords);
    }

    static CompletionOwner claim(NativeSocketBase socket) {
        CompletionOwner owner = socket.runtime().completionOwner();
        owner.transferToPublic(new Object());
        return owner;
    }

    static Throwable failure(CompletableFuture<?> future) throws Exception {
        ExecutionException exception = assertThrows(ExecutionException.class,
            () -> future.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS));
        return exception.getCause();
    }

    static void runProbe(Class<?> probe) throws Exception {
        Path java = Path.of(System.getProperty("java.home"), "bin", "java");
        Process process = new ProcessBuilder(java.toString(),
            "--enable-native-access=ALL-UNNAMED", "-cp", System.getProperty("java.class.path"),
            probe.getName()).redirectErrorStream(true).start();
        try {
            assertTrue(process.waitFor(20, TimeUnit.SECONDS), "completion probe must exit");
            String output = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
            assertEquals(0, process.exitValue(), output);
        } finally {
            process.destroyForcibly();
            process.waitFor(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
        }
    }
}
