package systems.zlink.bench.withgrpc.client;

import jdk.jfr.consumer.*;

import java.nio.file.*;
import java.util.*;

/** Full allocation events require -XX:-UseTLAB; this reader never treats lost data as zero cost. */
public final class RequestBackpressureAllocations {
    static String owner(RecordedStackTrace stack) {
        for (var frame : stack.getFrames()) {
            var method = frame.getMethod();
            String type = method.getType().getName();
            if (type.startsWith("systems.zlink.framework."))
                return "framework:" + type + "." + method.getName();
            if (type.startsWith("systems.zlink.runtime.")
                    || type.startsWith("systems.zlink.contracts."))
                return "binding:" + type + "." + method.getName();
        }
        return "other_including_harness";
    }

    public static void main(String[] args) throws Exception {
        long bytes = 0, count = 0, virtualBytes = 0, virtualCount = 0;
        var types = new HashMap<String, long[]>();
        var owners = new HashMap<String, long[]>();
        var categories = new TreeMap<String, long[]>();
        try (var file = new RecordingFile(Path.of(args[0]))) {
            while (file.hasMoreEvents()) {
                var event = file.readEvent();
                String eventName = event.getEventType().getName();
                if (eventName.equals("jdk.DataLoss"))
                    throw new AssertionError("JFR data loss: " + event);
                if (!eventName.equals("jdk.ObjectAllocationOutsideTLAB")) continue;
                var stack = event.getStackTrace();
                long size = event.getLong("allocationSize");
                bytes += size;
                count++;
                var thread = event.getThread();
                if (thread != null && thread.isVirtual()) {
                    virtualBytes += size;
                    virtualCount++;
                }
                String name = event.getClass("objectClass").getName();
                long[] costs = types.computeIfAbsent(name, n -> new long[2]);
                costs[0] += size;
                costs[1]++;
                String frameOwner =
                        stack == null || stack.isTruncated() ? "unknown_stack" : owner(stack);
                costs = owners.computeIfAbsent(frameOwner, n -> new long[2]);
                costs[0] += size;
                costs[1]++;
                String category =
                        frameOwner.equals("unknown_stack")
                                ? "unknown_stack"
                                : frameOwner.startsWith("framework:")
                                        ? "framework"
                                        : frameOwner.startsWith("binding:")
                                                ? "binding"
                                                : "other_including_harness";
                costs = categories.computeIfAbsent(category, n -> new long[4]);
                costs[0] += size;
                costs[1]++;
                if (thread != null && thread.isVirtual()) {
                    costs[2] += size;
                    costs[3]++;
                }
            }
        }
        if (count == 0) throw new AssertionError("No full allocation events");
        System.out.printf(
                "ALLOCATION file=%s bytes=%d objects=%d virtual_bytes=%d virtual_objects=%d"
                        + " includes_harness=true java_heap_only=true"
                        + " owner_attribution_excludes_unknown_stack=true%n",
                args[0], bytes, count, virtualBytes, virtualCount);
        categories.forEach(
                (name, costs) ->
                        System.out.printf(
                                "CATEGORY owner=%s bytes=%d objects=%d virtual_bytes=%d"
                                        + " virtual_objects=%d%n",
                                name, costs[0], costs[1], costs[2], costs[3]));
        owners.entrySet().stream()
                .sorted((a, b) -> Long.compare(b.getValue()[0], a.getValue()[0]))
                .limit(20)
                .forEach(
                        e ->
                                System.out.printf(
                                        "OWNER first_owner_frame=%s bytes=%d objects=%d%n",
                                        e.getKey(), e.getValue()[0], e.getValue()[1]));
        types.entrySet().stream()
                .sorted((a, b) -> Long.compare(b.getValue()[0], a.getValue()[0]))
                .limit(20)
                .forEach(
                        e ->
                                System.out.printf(
                                        "TYPE class=%s bytes=%d objects=%d%n",
                                        e.getKey(), e.getValue()[0], e.getValue()[1]));
    }
}
