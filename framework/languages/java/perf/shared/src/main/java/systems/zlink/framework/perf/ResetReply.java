package systems.zlink.framework.perf;

import java.util.Map;

public record ResetReply(
        boolean ok,
        String runId,
        String cellId,
        String role,
        int roleInstance,
        String resetSeq,
        String applicationResetAtUnixMs,
        String capacityEpoch,
        String reason,
        Map<String, NullReason> nullReasons) {}
