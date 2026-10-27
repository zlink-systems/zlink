package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.SendSubmission;
import systems.zlink.contracts.messaging.SendSubmitOperation;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceSendHeaderAllocationTest;

import java.nio.file.Files;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;

/** An immediate binding result does not require a captured admission accessor. */
public final class ZLinkSendSubmitAdapterAllocationTest {
    private static final int SENDS = 100;
    private static volatile Object observed;

    @Test
    void immediateSubmitDoesNotAllocateAnAdmissionAdapter() throws Exception {
        ZLinkServiceSendHeaderAllocationTest.runInAllocationJvm(getClass());
    }

    private static final class Immediate implements SendSubmission {
        @Override
        public SubmitResult result() {
            return SubmitResult.OK;
        }

        @Override
        public CompletionStage<Void> admitted() {
            throw new AssertionError("immediate admission accessor must remain unused");
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 2 && args[0].equals("--benchmark")) {
            systems.zlink.framework.runtime.internal.service.ZLinkServiceSendHeaderAllocationTest
                    .benchmarkPaths(
                            java.nio.file.Path.of(args[1]),
                            ZLinkSendSubmitAdapterAllocationTest.class,
                            java.util.List.of("immediate"),
                            100_000);
            return;
        }
        var immediate = new Immediate();
        SendSubmitOperation operation = operation(immediate);
        try (var operations = new ZLinkServiceOperationRegistry()) {
            for (int i = 0; i < 200; i++)
                observed = ZLinkJavaSocketSupport.submit(operation, operations);
            long control =
                    adapters(
                            () -> {
                                for (int i = 0; i < SENDS; i++)
                                    observed =
                                            (Supplier<CompletionStage<Void>>) immediate::admitted;
                            });
            long actual =
                    adapters(
                            () -> {
                                for (int i = 0; i < SENDS; i++)
                                    observed = ZLinkJavaSocketSupport.submit(operation, operations);
                            });
            System.out.printf(
                    "sends=%d controlAdapters=%d immediateAdapters=%d%n", SENDS, control, actual);
            assertEquals(SENDS, control, "the captured accessor is the same-JVM positive control");
            assertEquals(0, actual, "an immediate result needs no captured admission adapter");
        }
    }

    public static long[] measureSendPath(String path, int count) {
        var bean =
                (com.sun.management.ThreadMXBean)
                        java.lang.management.ManagementFactory.getThreadMXBean();
        var immediate = new Immediate();
        SendSubmitOperation operation = operation(immediate);
        try (var operations = new ZLinkServiceOperationRegistry()) {
            long bytes = bean.getCurrentThreadAllocatedBytes();
            long cpu = bean.getCurrentThreadCpuTime();
            long time = System.nanoTime();
            for (int i = 0; i < count; i++)
                observed = ZLinkJavaSocketSupport.submit(operation, operations);
            return new long[] {
                System.nanoTime() - time,
                bean.getCurrentThreadAllocatedBytes() - bytes,
                bean.getCurrentThreadCpuTime() - cpu
            };
        }
    }

    private static SendSubmitOperation operation(SendSubmission immediate) {
        return new SendSubmitOperation() {
            @Override
            public SendSubmitOperation message(Message message) {
                throw new AssertionError();
            }

            @Override
            public SendSubmission submit() {
                return immediate;
            }

            @Override
            public void submit_sync() {
                throw new AssertionError();
            }
        };
    }

    private static long adapters(Runnable action) throws Exception {
        var file = Files.createTempFile("zlink-send-adapter-", ".jfr");
        try {
            try (var recording = new Recording()) {
                recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                recording.enable("jdk.DataLoss");
                recording.start();
                action.run();
                recording.stop();
                recording.dump(file);
            }
            long count = 0;
            try (var events = new RecordingFile(file)) {
                while (events.hasMoreEvents()) {
                    var event = events.readEvent();
                    assertNotEquals("jdk.DataLoss", event.getEventType().getName());
                    if (event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB")
                            && event.getClass("objectClass").getName().contains("$$Lambda")
                            && event.getStackTrace() != null
                            && event.getStackTrace().getFrames().stream()
                                    .anyMatch(
                                            frame ->
                                                    frame.getMethod()
                                                                    .getType()
                                                                    .getName()
                                                                    .equals(
                                                                            ZLinkSendSubmitAdapterAllocationTest
                                                                                    .class
                                                                                    .getName())
                                                            && frame.getMethod()
                                                                    .getName()
                                                                    .startsWith("lambda$main$")))
                        count++;
                }
            }
            return count;
        } finally {
            Files.deleteIfExists(file);
        }
    }
}
