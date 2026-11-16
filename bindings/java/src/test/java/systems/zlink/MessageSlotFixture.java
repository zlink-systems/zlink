/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.nio.file.Path;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import systems.zlink.internal.ContractAccess;
import systems.zlink.contracts.errors.ConfigResult;

import static org.junit.jupiter.api.Assertions.*;

/** Observes the existing native bridge before Message initializes in a probe JVM. */
public final class MessageSlotFixture {
    public final AtomicInteger opens = new AtomicInteger();
    public final AtomicInteger scopeCloses = new AtomicInteger();
    public final Map<Long, AtomicInteger> payloadCloses = new ConcurrentHashMap<>();
    public volatile String failOperation;
    public volatile RuntimeException closeFailure;
    public volatile boolean rejectClose;
    public volatile long failedHeader;
    public volatile int failureErrno;
    public volatile int closesAtFailure;
    private final Map<Long, Object> headerScopes = new ConcurrentHashMap<>();
    private final ContractAccess.NativeMessageAccess delegate;

    public MessageSlotFixture() {
        delegate = ContractAccess.nativeMessageAccessForRuntime();
        ContractAccess.register((ContractAccess.NativeMessageAccess) Proxy.newProxyInstance(
            getClass().getClassLoader(), new Class<?>[] {ContractAccess.NativeMessageAccess.class},
            (proxy, method, args) -> {
                String operation = method.getName();
                if (operation.equals("openSharedMessageScope")) opens.incrementAndGet();
                if (operation.equals("closeMessageScope")) scopeCloses.incrementAndGet();
                if (operation.equals("close")) {
                    if (closeFailure != null) throw closeFailure;
                    if (rejectClose) return ConfigResult.INTERNAL_ERROR.value();
                    payloadCloses.computeIfAbsent(delegate.address(args[0]),
                        ignored -> new AtomicInteger()).incrementAndGet();
                }
                if (operation.equals(failOperation)) {
                    failedHeader = delegate.address(args[0]);
                    failureErrno = ContractAccess.nativeErrno();
                    closesAtFailure = closes(failedHeader);
                    failOperation = null;
                    return ConfigResult.INVALID_ARGUMENT.value();
                }
                try {
                    Object result = method.invoke(delegate, args);
                    if (operation.equals("allocate")) headerScopes.put(delegate.address(result), args[0]);
                    return result;
                } catch (InvocationTargetException failure) {
                    throw failure.getCause();
                }
            }));
    }

    public int closes(long address) {
        AtomicInteger count = payloadCloses.get(address);
        return count == null ? 0 : count.get();
    }

    public void releaseQuarantinedHeader() {
        Object header = delegate.handleFromAddress(failedHeader);
        assertEquals(0, delegate.close(header));
        delegate.closeMessageScope(headerScopes.remove(failedHeader));
    }

    public static void runProbe(Class<?> probe) throws Exception {
        Path java = Path.of(System.getProperty("java.home"), "bin", "java");
        Process process = new ProcessBuilder(java.toString(), "--enable-native-access=ALL-UNNAMED",
            "-cp", System.getProperty("java.class.path"), probe.getName())
            .redirectErrorStream(true).start();
        try {
            assertTrue(process.waitFor(60, TimeUnit.SECONDS), "message probe must exit");
            String output = new String(process.getInputStream().readAllBytes(),
                StandardCharsets.UTF_8);
            assertEquals(0, process.exitValue(), output);
            System.out.print(output);
        } finally {
            process.destroyForcibly();
            process.waitFor(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
        }
    }
}
