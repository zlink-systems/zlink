package systems.zlink.framework.perf;

import java.util.List;

// §16.1
public record PerfReady(
        String runId,
        String cellId,
        String role,
        int roleInstance,
        boolean infrastructureReady,
        boolean objectsReady,
        boolean consumersReady,
        boolean ready,
        String observedAtUnixMs,
        List<Object> evidence,
        List<String> reasons) {}
