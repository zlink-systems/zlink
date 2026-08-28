package systems.zlink.framework.runtime.streams;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;

/** Test-only caller for the package-owned STREAM session execution entrypoints. */
public final class ZLinkSessionSerialExecutorContractProbe {
    private ZLinkSessionSerialExecutorContractProbe() {
    }

    public static List<String> executeEverySubmissionPath(Executor executor)
        throws Exception {
        ZLinkSessionSerialExecutor serial = new ZLinkSessionSerialExecutor(executor);
        List<String> executed = new CopyOnWriteArrayList<>();

        CompletableFuture.allOf(
            serial.executeApplication(() -> completed(executed, "application"))
                .toCompletableFuture(),
            serial.executeControl(() -> completed(executed, "control"))
                .toCompletableFuture(),
            serial.executeInfrastructure(() -> completed(executed, "infrastructure"))
                .toCompletableFuture(),
            serial.executeFinal(() -> completed(executed, "final"))
                .toCompletableFuture())
            .get(3, TimeUnit.SECONDS);
        return List.copyOf(executed);
    }

    private static CompletableFuture<Void> completed(
        List<String> executed,
        String entrypoint) {
        executed.add(entrypoint);
        return CompletableFuture.completedFuture(null);
    }
}
