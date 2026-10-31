package systems.zlink.framework.execution;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import jdk.jfr.Event;
import jdk.jfr.EventType;
import jdk.jfr.Name;
import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordedEvent;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import java.net.URLClassLoader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.concurrent.CompletableFuture;

public final class ZLinkSerialTurnAllocationTest {
    private static final int UNFILTERED_DISPATCHES = 1000;
    // #1673 dispatch loop: 39881 objects / 1556976 bytes over 1000 dispatches.
    // Round each per-dispatch mean to the nearest integer; .5 rounds up (Math.round).
    private static final long EXPECTED_OBJECTS_PER_DISPATCH = 40;
    private static final long EXPECTED_BYTES_PER_DISPATCH = 1557;
    private static final String UNFILTERED_WINDOW_EVENT = "systems.zlink.UnfilteredDispatchWindow";

    @Name(UNFILTERED_WINDOW_EVENT)
    public static final class UnfilteredDispatchWindow extends Event {}

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
        String currentClasspath = String.join(java.io.File.pathSeparator, classpath);
        long[] actual = allocationTotals(runProbe(currentClasspath, false));
        assertUnfilteredAllocationBudget(actual);
        long[] mutation = allocationTotals(runProbe(currentClasspath, true));
        assertEquals(
                actual[0] + UNFILTERED_DISPATCHES,
                mutation[0],
                "one added object per dispatch must be detected");
        assertTrue(mutation[1] > actual[1], "the added objects must increase allocated bytes");
        assertThrows(
                AssertionError.class,
                () -> assertUnfilteredAllocationBudget(mutation),
                "the same allocation budget must reject one added object per dispatch");
        System.out.println(
                "one-object negative control: objects=+"
                        + (mutation[0] - actual[0])
                        + " bytes=+"
                        + (mutation[1] - actual[1]));
    }

    private static void assertUnfilteredAllocationBudget(long[] totals) {
        assertEquals(
                EXPECTED_OBJECTS_PER_DISPATCH,
                Math.round((double) totals[0] / UNFILTERED_DISPATCHES),
                "rounded objects per unfiltered dispatch");
        assertEquals(
                EXPECTED_BYTES_PER_DISPATCH,
                Math.round((double) totals[1] / UNFILTERED_DISPATCHES),
                "rounded bytes per unfiltered dispatch");
    }

    private String runProbe(String classpath, boolean addObject) throws Exception {
        var output = Files.createTempFile("zlink-turn-allocation", ".log");
        try {
            var process =
                    new ProcessBuilder(
                                    Path.of(System.getProperty("java.home"), "bin", "java")
                                            .toString(),
                                    "--enable-native-access=ALL-UNNAMED",
                                    "-Xint",
                                    "-XX:-UseTLAB",
                                    "-XX:FlightRecorderOptions=stackdepth=128",
                                    "-cp",
                                    classpath,
                                    getClass().getName(),
                                    Boolean.toString(addObject))
                            .redirectErrorStream(true)
                            .redirectOutput(output.toFile())
                            .start();
            int exit = process.waitFor();
            String log = Files.readString(output);
            System.out.print(log);
            assertEquals(0, exit, log);
            return log;
        } finally {
            Files.deleteIfExists(output);
        }
    }

    private static long[] allocationTotals(String log) {
        var totals =
                java.util.regex.Pattern.compile(
                                "channel unfiltered allocation totals: objects=(\\d+) bytes=(\\d+)")
                        .matcher(log);
        assertTrue(totals.find(), "allocation totals must be present");
        return new long[] {Long.parseLong(totals.group(1)), Long.parseLong(totals.group(2))};
    }

    private static volatile Object extraAllocation;

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
            var unfiltered =
                    systems.zlink.framework.runtime.channels.ZLinkChannelDispatchStageBudgetTest
                            .allocationFixture(false);
            var rejected =
                    systems.zlink.framework.runtime.channels.ZLinkChannelDispatchStageBudgetTest
                            .rejectingAllocationFixture();
            var filtered =
                    systems.zlink.framework.runtime.channels.ZLinkChannelDispatchStageBudgetTest
                            .allocationFixture(true);
            for (int i = 0; i < 20; i++) {
                unfiltered.run();
                filtered.run();
                rejected.run();
            }
            var unfilteredWindow = new UnfilteredDispatchWindow();
            var unfilteredWindowType = EventType.getEventType(UnfilteredDispatchWindow.class);
            var unfilteredWork =
                    new java.util.concurrent.FutureTask<Void>(
                            () -> {
                                unfilteredWindow.begin();
                                for (int i = 0; i < UNFILTERED_DISPATCHES; i++) {
                                    unfiltered.run();
                                    if (args.length != 0 && Boolean.parseBoolean(args[0]))
                                        extraAllocation = new Object();
                                }
                                unfilteredWindow.end();
                                unfilteredWindow.commit();
                                return null;
                            });
            var filteredWork =
                    new java.util.concurrent.FutureTask<Void>(
                            () -> {
                                for (int i = 0; i < 1000; i++) filtered.run();
                                return null;
                            });
            var rejectedWork =
                    new java.util.concurrent.FutureTask<Void>(
                            () -> {
                                for (int i = 0; i < 1000; i++) rejected.run();
                                return null;
                            });
            var rejectedThread = new Thread(rejectedWork);
            var unfilteredThread = new Thread(unfilteredWork);
            var filteredThread = new Thread(filteredWork);
            try {
                for (int i = 0; i < 20; i++) runTurn(queue, executor, completed);
                queue.awaitQuiescence().toCompletableFuture().join();
                recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                recording
                        .enable(unfilteredWindowType.getName())
                        .withThreshold(java.time.Duration.ZERO)
                        .withoutStackTrace();
                recording.start();
                for (int i = 0; i < 1000; i++) observeCompleted(completed);
                for (int i = 0; i < 1000; i++) runTurn(queue, executor, completed);
                queue.awaitQuiescence().toCompletableFuture().join();
                unfilteredThread.start();
                unfilteredThread.join();
                unfilteredWork.get();
                filteredThread.start();
                filteredThread.join();
                filteredWork.get();
                rejectedThread.start();
                rejectedThread.join();
                rejectedWork.get();
                recording.stop();
                recording.dump(recordingPath);
            } finally {
                queue.close();
            }
            RecordedEvent dispatchWindow = null;
            try (var events = new RecordingFile(recordingPath)) {
                while (events.hasMoreEvents()) {
                    var event = events.readEvent();
                    if (event.getEventType().getName().equals(UNFILTERED_WINDOW_EVENT)) {
                        assertTrue(
                                dispatchWindow == null,
                                "one unfiltered dispatch window is required");
                        assertEquals(
                                unfilteredThread.threadId(),
                                event.getThread().getJavaThreadId(),
                                "the dispatch thread must own the measurement window");
                        dispatchWindow = event;
                    }
                }
            }
            assertTrue(dispatchWindow != null, "the unfiltered dispatch window must be recorded");
            assertTrue(
                    dispatchWindow.getEndTime().isAfter(dispatchWindow.getStartTime()),
                    "the dispatch window must have a positive duration");
            long futures = 0;
            long baseline = 0;
            long actions = 0;
            long iterators = 0;
            long turnContextEntries = 0;
            long unfilteredObjects = 0;
            long unfilteredTotalBytes = 0;
            long unfilteredOwnerContainers = 0;
            long unfilteredActivations = 0;
            long unfilteredActions = 0;
            long unfilteredBytes = 0;
            long filteredActions = 0;
            long filteredBytes = 0;
            long unfilteredArguments = 0;
            long filteredArguments = 0;
            long rejectedArguments = 0;
            try (var events = new RecordingFile(recordingPath)) {
                while (events.hasMoreEvents()) {
                    var event = events.readEvent();
                    if (!event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB"))
                        continue;
                    String allocatedClass = event.getClass("objectClass").getName();
                    if (event.getThread("eventThread").getJavaThreadId()
                            == unfilteredThread.threadId()) {
                        if (!event.getStartTime().isBefore(dispatchWindow.getStartTime())
                                && !event.getStartTime().isAfter(dispatchWindow.getEndTime())) {
                            unfilteredObjects++;
                            unfilteredTotalBytes += event.getLong("allocationSize");
                        }
                        if (allocatedClass.equals(
                                        "systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner")
                                || allocatedClass.equals("java.util.LinkedHashMap")
                                || allocatedClass.startsWith(
                                        "systems.zlink.framework.runtime.internal.execution.ZLinkStateLane"))
                            unfilteredOwnerContainers++;
                        if (allocatedClass.equals(
                                "systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner$SingleActivation"))
                            unfilteredActivations++;
                    }
                    if (allocatedClass.equals("[Ljava.lang.Object;")) {
                        var stack = event.getStackTrace().getFrames();
                        var owner =
                                stack.stream()
                                        .filter(
                                                frame ->
                                                        frame.getMethod()
                                                                .getType()
                                                                .getName()
                                                                .startsWith(
                                                                        "systems.zlink.framework."))
                                        .findFirst();
                        boolean backtrace =
                                stack.stream()
                                        .anyMatch(
                                                frame ->
                                                        frame.getMethod()
                                                                        .getType()
                                                                        .getName()
                                                                        .equals(
                                                                                Throwable.class
                                                                                        .getName())
                                                                && frame.getMethod()
                                                                        .getName()
                                                                        .equals(
                                                                                "fillInStackTrace"));
                        if (!backtrace
                                && owner.isPresent()
                                && owner.get()
                                        .getMethod()
                                        .getType()
                                        .getName()
                                        .equals(
                                                "systems.zlink.framework.runtime.channels.ZLinkChannelHandlerInvoker")) {
                            long thread = event.getThread("eventThread").getJavaThreadId();
                            if (thread == unfilteredThread.threadId()) unfilteredArguments++;
                            else if (thread == filteredThread.threadId()) filteredArguments++;
                            else if (thread == rejectedThread.threadId()) rejectedArguments++;
                        }
                    }
                    if (allocatedClass.startsWith(
                            "systems.zlink.framework.runtime.channels.ZLinkChannelHandlerInvoker$$Lambda")) {
                        if (event.getThread("eventThread").getJavaThreadId()
                                == unfilteredThread.threadId()) {
                            unfilteredActions++;
                            unfilteredBytes += event.getLong("allocationSize");
                        } else if (event.getThread("eventThread").getJavaThreadId()
                                == filteredThread.threadId()) {
                            filteredActions++;
                            filteredBytes += event.getLong("allocationSize");
                        }
                    }
                    if (allocatedClass.equals("java.lang.ThreadLocal$ThreadLocalMap$Entry")) {
                        var owner =
                                event.getStackTrace().getFrames().stream()
                                        .filter(
                                                frame ->
                                                        frame.getMethod()
                                                                .getType()
                                                                .getName()
                                                                .startsWith(
                                                                        "systems.zlink.framework."))
                                        .findFirst();
                        if (owner.isPresent()
                                && owner.get()
                                        .getMethod()
                                        .getType()
                                        .getName()
                                        .equals(ZLinkSerialExecutionQueue.class.getName())
                                && owner.get().getMethod().getName().equals("invokeInline"))
                            turnContextEntries++;
                    }
                    if (allocatedClass.startsWith("java.util.")
                            && !allocatedClass.contains("$$Lambda")
                            && java.util.Iterator.class.isAssignableFrom(
                                    Class.forName(allocatedClass))
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
                                                                    .equals("finish"))) iterators++;
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
            System.out.println("serial finish Iterator allocations: " + iterators + "/1000");
            System.out.println(
                    "serial physical context ThreadLocal entries: " + turnContextEntries + "/1000");
            System.out.println(
                    "channel unfiltered owner actions: "
                            + unfilteredActions
                            + "/1000; bytes="
                            + unfilteredBytes);
            System.out.println(
                    "channel filtered owner actions: "
                            + filteredActions
                            + "/1000; bytes="
                            + filteredBytes);
            System.out.println(
                    "channel invocation argument arrays: unfiltered="
                            + unfilteredArguments
                            + " filtered="
                            + filteredArguments
                            + " rejected="
                            + rejectedArguments
                            + "/1000");
            long plainArguments = unfilteredArguments;
            long permittedArguments = filteredArguments;
            long skippedArguments = rejectedArguments;
            System.out.println(
                    "channel unfiltered owner/map/lane allocations: "
                            + unfilteredOwnerContainers
                            + "/1000; single activations="
                            + unfilteredActivations);
            System.out.println(
                    "channel unfiltered allocation totals: objects="
                            + unfilteredObjects
                            + " bytes="
                            + unfilteredTotalBytes);
            long ownerContainers = unfilteredOwnerContainers;
            long singleActivations = unfilteredActivations;
            long channelActions = unfilteredActions;
            long contextEntries = turnContextEntries;
            long finishIterators = iterators;
            long submittedActions = actions;
            long turnFutures = futures - baseline;
            org.junit.jupiter.api.Assertions.assertAll(
                    () ->
                            assertEquals(
                                    0,
                                    ownerContainers,
                                    "unfiltered dispatch needs no owner/map/lane"),
                    () ->
                            assertEquals(
                                    0,
                                    singleActivations,
                                    "successful unfiltered dispatch needs no lease or release"
                                            + " record"),
                    () ->
                            assertEquals(
                                    1000,
                                    plainArguments,
                                    "one invocation argument pack per unfiltered handler"),
                    () ->
                            assertEquals(
                                    1000,
                                    permittedArguments,
                                    "one invocation argument pack per filter-permitted handler"),
                    () ->
                            assertEquals(
                                    0,
                                    skippedArguments,
                                    "a skipped handler must not prepare invocation arguments"),
                    () ->
                            assertEquals(
                                    2000,
                                    channelActions,
                                    "unfiltered dispatch only needs reply mapping and terminal"
                                            + " scope cleanup actions"),
                    () ->
                            assertEquals(
                                    1000,
                                    contextEntries,
                                    "one physical turn context entry per turn"),
                    () ->
                            assertEquals(
                                    0,
                                    finishIterators,
                                    "empty quiescence notification must not allocate an iterator"),
                    () ->
                            assertEquals(
                                    2000,
                                    submittedActions,
                                    "one drain and one rejection action per submission"),
                    () ->
                            assertEquals(
                                    1000,
                                    turnFutures,
                                    "one execution gate per turn beyond the JDK observer"
                                            + " baseline"));
        } finally {
            Files.deleteIfExists(recordingPath);
        }
    }
}
