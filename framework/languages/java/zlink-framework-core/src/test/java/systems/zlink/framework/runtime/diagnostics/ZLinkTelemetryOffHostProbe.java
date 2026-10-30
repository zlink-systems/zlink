package systems.zlink.framework.runtime.diagnostics;

import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;

/** A fresh JVM proves that an Off-only host never starts a telemetry execution resource. */
public final class ZLinkTelemetryOffHostProbe {
    public static void main(String[] args) {
        var options = new DefaultZLinkFrameworkOptions();
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.OFF);
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            requireNoWorker();
        }
        requireNoWorker();
        System.out.println("OFF_HOST_TELEMETRY_THREADS=0");
    }

    private static void requireNoWorker() {
        if (Thread.getAllStackTraces().keySet().stream()
                .anyMatch(thread -> thread.getName().equals("zlink-process-telemetry"))) {
            throw new AssertionError("Off host created telemetry worker");
        }
    }
}
