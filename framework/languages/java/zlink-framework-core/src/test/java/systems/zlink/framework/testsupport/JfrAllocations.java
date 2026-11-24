package systems.zlink.framework.testsupport;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordedFrame;
import jdk.jfr.consumer.RecordingFile;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Predicate;

/**
 * Records every heap allocation made while an action runs. Child JVM fixtures start with TLAB
 * disabled ({@link ChildJvm#runInAllocationJvm}), so each allocation is one event and counts are
 * exact.
 */
public final class JfrAllocations {
    private JfrAllocations() {}

    /** Runs {@code fixture.main(args)} in a child JVM with TLAB and escape analysis disabled. */
    public static void runInChild(Class<?> fixture, String... args) throws Exception {
        ChildJvm.runInJvm(
                fixture,
                java.util.List.of(
                        "-Djava.io.tmpdir=" + System.getProperty("java.io.tmpdir"),
                        "-XX:-UseTLAB",
                        "-XX:-DoEscapeAnalysis",
                        "-XX:ActiveProcessorCount=2",
                        "-XX:FlightRecorderOptions=stackdepth=128"),
                args);
    }

    @FunctionalInterface
    public interface Action {
        void run() throws Exception;
    }

    /** One allocation: its class name and its stack as {@code fully.qualified.Type.method}. */
    public record Event(String type, List<String> frames) {
        /** The innermost frame owned by the Framework, which is the code that allocated. */
        public String frameworkFrame() {
            for (String frame : frames) {
                if (frame.startsWith("systems.zlink.framework.")) return frame;
            }
            return "";
        }

        public boolean passesThrough(String owner, String method) {
            return frames.contains(owner + "." + method);
        }

        public boolean passesThroughType(String owner) {
            for (String frame : frames) {
                if (frame.startsWith(owner + ".")) return true;
            }
            return false;
        }
    }

    public static List<Event> record(Action action) throws Exception {
        Path file = Files.createTempFile("zlink-allocations-", ".jfr");
        try {
            try (var recording = new Recording()) {
                recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                recording.enable("jdk.DataLoss");
                recording.start();
                try {
                    action.run();
                } finally {
                    recording.stop();
                    recording.dump(file);
                }
            }
            var events = new ArrayList<Event>();
            try (var reader = new RecordingFile(file)) {
                while (reader.hasMoreEvents()) {
                    var event = reader.readEvent();
                    String name = event.getEventType().getName();
                    if (name.equals("jdk.DataLoss")) throw new AssertionError("JFR lost events");
                    if (!name.equals("jdk.ObjectAllocationOutsideTLAB")
                            || event.getStackTrace() == null) continue;
                    var frames = new ArrayList<String>();
                    for (RecordedFrame frame : event.getStackTrace().getFrames()) {
                        frames.add(
                                frame.getMethod().getType().getName()
                                        + "."
                                        + frame.getMethod().getName());
                    }
                    events.add(new Event(event.getClass("objectClass").getName(), frames));
                }
            }
            return events;
        } finally {
            Files.deleteIfExists(file);
        }
    }

    public static long count(List<Event> events, Predicate<Event> filter) {
        return events.stream().filter(filter).count();
    }
}
