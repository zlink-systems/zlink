package systems.zlink.framework.execution;

import static org.junit.jupiter.api.Assertions.assertEquals;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import java.net.URLClassLoader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.concurrent.CompletableFuture;

public final class ZLinkSerialTurnAllocationTest {
    @Test
    void synchronousTurnsAllocateOnlyTheirGateAndCompletionObserver() throws Exception {
        var classpath = new LinkedHashSet<String>();
        for (ClassLoader loader = getClass().getClassLoader();
                loader != null;
                loader = loader.getParent()) {
            if (loader instanceof URLClassLoader urls) {
                for (var url : urls.getURLs()) classpath.add(Path.of(url.toURI()).toString());
            }
        }
        classpath.addAll(
                List.of(System.getProperty("java.class.path").split(java.io.File.pathSeparator)));
        var output = Files.createTempFile("zlink-turn-allocation", ".log");
        try {
            var process =
                    new ProcessBuilder(
                                    Path.of(System.getProperty("java.home"), "bin", "java")
                                            .toString(),
                                    "-Xint",
                                    "-XX:-UseTLAB",
                                    "-XX:FlightRecorderOptions=stackdepth=128",
                                    "-cp",
                                    String.join(java.io.File.pathSeparator, classpath),
                                    getClass().getName())
                            .redirectErrorStream(true)
                            .redirectOutput(output.toFile())
                            .start();
            int exit = process.waitFor();
            String log = Files.readString(output);
            System.out.print(log);
            assertEquals(0, exit, log);
        } finally {
            Files.deleteIfExists(output);
        }
    }

    private static void observeCompleted(CompletableFuture<Void> completed) {
        completed.whenComplete((value, failure) -> {});
    }

    private static void runTurn(
            ZLinkSerialExecutionQueue queue,
            ManualExecutor executor,
            CompletableFuture<Void> completed) {
        var result = queue.enqueue(() -> completed, null);
        executor.pending.removeFirst().run();
        result.toCompletableFuture().join();
    }

    private static final class ManualExecutor extends java.util.concurrent.AbstractExecutorService {
        final java.util.ArrayDeque<Runnable> pending = new java.util.ArrayDeque<>();
        boolean shutdown;

        @Override
        public void execute(Runnable command) {
            pending.addLast(command);
        }

        @Override
        public void shutdown() {
            shutdown = true;
        }

        @Override
        public List<Runnable> shutdownNow() {
            shutdown();
            return List.of();
        }

        @Override
        public boolean isShutdown() {
            return shutdown;
        }

        @Override
        public boolean isTerminated() {
            return shutdown && pending.isEmpty();
        }

        @Override
        public boolean awaitTermination(long timeout, java.util.concurrent.TimeUnit unit) {
            return isTerminated();
        }
    }

    public static void main(String[] args) throws Exception {
        Path recordingPath = Files.createTempFile("zlink-turn-allocation", ".jfr");
        try (var executor = new ManualExecutor();
                var recording = new Recording()) {
            var queue = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.generic());
            var completed = CompletableFuture.<Void>completedFuture(null);
            try {
                for (int i = 0; i < 20; i++) runTurn(queue, executor, completed);
                queue.awaitQuiescence().toCompletableFuture().join();
                recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                recording.start();
                for (int i = 0; i < 1000; i++) observeCompleted(completed);
                for (int i = 0; i < 1000; i++) runTurn(queue, executor, completed);
                queue.awaitQuiescence().toCompletableFuture().join();
                recording.stop();
                recording.dump(recordingPath);
            } finally {
                queue.close();
            }
            long futures = 0;
            long baseline = 0;
            long actions = 0;
            try (var events = new RecordingFile(recordingPath)) {
                while (events.hasMoreEvents()) {
                    var event = events.readEvent();
                    if (!event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB"))
                        continue;
                    if (event.getClass("objectClass")
                                    .getName()
                                    .startsWith(
                                            ZLinkSerialExecutionQueue.class.getName() + "$$Lambda")
                            && event.getStackTrace().getFrames().stream()
                                    .anyMatch(
                                            frame ->
                                                    frame.getMethod()
                                                                    .getType()
                                                                    .getName()
                                                                    .equals(
                                                                            ZLinkSerialExecutionQueue
                                                                                    .class
                                                                                    .getName())
                                                            && frame.getMethod()
                                                                    .getName()
                                                                    .equals(
                                                                            "scheduleDrainIfNeeded")))
                        actions++;
                    if (!event.getClass("objectClass")
                            .getName()
                            .equals(CompletableFuture.class.getName())) continue;
                    if (event.getStackTrace().getFrames().stream()
                            .anyMatch(
                                    frame ->
                                            frame.getMethod()
                                                            .getType()
                                                            .getName()
                                                            .equals(
                                                                    ZLinkSerialExecutionQueue.class
                                                                            .getName())
                                                    && frame.getMethod()
                                                            .getName()
                                                            .equals("invokeInline"))) futures++;
                    if (event.getStackTrace().getFrames().stream()
                            .anyMatch(
                                    frame ->
                                            frame.getMethod()
                                                            .getType()
                                                            .getName()
                                                            .equals(
                                                                    ZLinkSerialTurnAllocationTest
                                                                            .class
                                                                            .getName())
                                                    && frame.getMethod()
                                                            .getName()
                                                            .equals("observeCompleted")))
                        baseline++;
                }
            }
            System.out.println(
                    "serial turn CompletableFuture allocations: "
                            + futures
                            + "/1000; completion observer baseline="
                            + baseline);
            System.out.println("serial worker submission actions: " + actions + "/1000");
            long submittedActions = actions;
            long turnFutures = futures - baseline;
            org.junit.jupiter.api.Assertions.assertAll(
                    () ->
                            assertEquals(
                                    2000,
                                    submittedActions,
                                    "one drain and one rejection action per submission"),
                    () ->
                            assertEquals(
                                    1000,
                                    turnFutures,
                                    "one execution gate per turn beyond the JDK observer baseline"));
        } finally {
            Files.deleteIfExists(recordingPath);
        }
    }
}
