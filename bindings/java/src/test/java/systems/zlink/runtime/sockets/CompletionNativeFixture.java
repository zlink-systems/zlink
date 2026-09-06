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
        replace(replacements, "zlink_send_part", "send", FunctionDescriptor.of(JAVA_INT,
            ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS));
        replace(replacements, "zlink_send_part_rid", "sendRid", FunctionDescriptor.of(JAVA_INT,
            ADDRESS, ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS));
        replace(replacements, "zlink_request_part", "request", FunctionDescriptor.of(JAVA_INT,
            ADDRESS, ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS));
        replace(replacements, "zlink_reply_part", "reply", FunctionDescriptor.of(JAVA_INT,
            ADDRESS, ADDRESS, JAVA_LONG, ADDRESS, JAVA_INT));
        replace(replacements, "zlink_disconnect_rid", "disconnect", FunctionDescriptor.of(JAVA_INT,
            ADDRESS, ADDRESS));
        replace(replacements, "zlink_completion_recv", "recv", FunctionDescriptor.of(JAVA_INT,
            ADDRESS, ADDRESS, JAVA_INT));
        replace(replacements, "zlink_completion_close", "closeRecord", FunctionDescriptor.ofVoid(ADDRESS));
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

    private void errno(int value) throws Throwable {
        ((MemorySegment) errnoLocation.invokeExact()).reinterpret(JAVA_INT.byteSize())
            .set(JAVA_INT, 0, value);
    }

    private int submit(MemorySegment part, int partFlag, MemorySegment context,
                       MemorySegment idOut, boolean request, MemorySegment rid) throws Throwable {
        NativeMessage.messageClose(part);
        if (partFlag == Native.PART_MORE)
            return SubmitResult.OK.value();
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

    private int send(MemorySegment socket, MemorySegment part, int flags, int partFlag,
                     MemorySegment context, MemorySegment idOut) throws Throwable {
        return submit(part, partFlag, context, idOut, false, MemorySegment.NULL);
    }

    private int sendRid(MemorySegment socket, MemorySegment rid, MemorySegment part,
                        int flags, int partFlag, MemorySegment context,
                        MemorySegment idOut) throws Throwable {
        return submit(part, partFlag, context, idOut, false, rid);
    }

    private int request(MemorySegment socket, MemorySegment rid, MemorySegment part,
                        int flags, int partFlag, int timeout, MemorySegment context,
                        MemorySegment idOut) throws Throwable {
        return submit(part, partFlag, context, idOut, true, rid);
    }

    private int reply(MemorySegment socket, MemorySegment rid, long token,
                      MemorySegment part, int partFlag) throws Throwable {
        return submit(part, partFlag, MemorySegment.NULL, MemorySegment.NULL, false, rid);
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
