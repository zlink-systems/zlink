package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.sun.jdi.Bootstrap;
import com.sun.jdi.event.BreakpointEvent;
import com.sun.jdi.event.ClassPrepareEvent;
import com.sun.jdi.event.EventSet;
import com.sun.jdi.request.BreakpointRequest;
import com.sun.jdi.request.EventRequest;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.net.URLClassLoader;
import java.nio.file.Path;
import java.time.Duration;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Spec 32 §6 and Java §10: connection admission is atomic and CLOSED is terminal. */
final class ZLinkStreamLifecycleRaceTest {
    private static final long WAIT_SECONDS = 10;
    private static final String LATE_CONNECT_THREAD = "late-connect";

    /**
     * JDI pauses only the racing caller, before the lifecycle lock is acquired. The pause is a
     * breakpoint on the first instruction of the boundary method, not a method-entry request: a
     * method-entry request runs every thread of the probe in interpreter-only mode and reports
     * every call to the debugger agent, which stretches the probe's own first connect past its
     * connect timeout under load. The breakpoint slows only the boundary method. The checkpoint
     * latch proves that the other caller completed connect/close before the paused path resumes. No
     * runtime hook or private-field inspection is needed; the probe observes public API results and
     * the connections accepted by the server.
     */
    @ParameterizedTest
    @ValueSource(strings = {"connect", "receiveFailure", "serverClosing"})
    void lifecycleRaceHasOneAdmissionAndATerminalClose(String scenario) throws Exception {
        var launcher = Bootstrap.virtualMachineManager().defaultConnector();
        var arguments = launcher.defaultArguments();
        arguments.get("main").setValue(Probe.class.getName() + " " + scenario);
        arguments.get("options").setValue("-cp \"" + testClasspath() + "\"");
        var vm = launcher.launch(arguments);
        Process process = vm.process();
        CountDownLatch checkpoint = new CountDownLatch(1);
        StringBuffer output = new StringBuffer();
        Thread reader =
                Thread.ofPlatform()
                        .start(
                                () -> {
                                    try (var lines =
                                            new BufferedReader(
                                                    new InputStreamReader(
                                                            process.getInputStream()))) {
                                        String line;
                                        while ((line = lines.readLine()) != null) {
                                            output.append(line).append('\n');
                                            if (line.equals("CHECKPOINT")) {
                                                checkpoint.countDown();
                                            }
                                        }
                                    } catch (Exception failure) {
                                        output.append(failure);
                                    }
                                });
        try {
            String boundary =
                    scenario.equals("connect") ? "startConnectionAttempt" : "reconnectOrDisconnect";
            BreakpointRequest entry = null;
            var preparation = vm.eventRequestManager().createClassPrepareRequest();
            preparation.addClassFilter(ZLinkStreamConnectionLifecycle.class.getName());
            preparation.setSuspendPolicy(EventRequest.SUSPEND_EVENT_THREAD);
            preparation.enable();
            var transportPreparation = vm.eventRequestManager().createClassPrepareRequest();
            transportPreparation.addClassFilter(ZLinkNettyTransportConnection.class.getName());
            transportPreparation.setSuspendPolicy(EventRequest.SUSPEND_EVENT_THREAD);
            transportPreparation.enable();
            int transportAttempts = 0;
            EventSet paused = null;
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(WAIT_SECONDS);
            while (paused == null && System.nanoTime() < deadline) {
                EventSet events;
                try {
                    events = vm.eventQueue().remove(1000);
                } catch (com.sun.jdi.VMDisconnectedException failure) {
                    process.waitFor(WAIT_SECONDS, TimeUnit.SECONDS);
                    reader.join(TimeUnit.SECONDS.toMillis(WAIT_SECONDS));
                    throw new AssertionError(
                            output
                                    + new String(
                                            process.getErrorStream().readAllBytes(),
                                            java.nio.charset.StandardCharsets.UTF_8),
                            failure);
                }
                if (events == null) {
                    continue;
                }
                for (var event : events) {
                    if (event instanceof ClassPrepareEvent prepared
                            && prepared.referenceType()
                                    .name()
                                    .equals(ZLinkNettyTransportConnection.class.getName())) {
                        var opening =
                                vm.eventRequestManager()
                                        .createBreakpointRequest(
                                                prepared.referenceType()
                                                        .methodsByName("connectStage")
                                                        .getFirst()
                                                        .location());
                        opening.setSuspendPolicy(EventRequest.SUSPEND_NONE);
                        opening.enable();
                        transportPreparation.disable();
                    } else if (event instanceof ClassPrepareEvent prepared) {
                        entry =
                                vm.eventRequestManager()
                                        .createBreakpointRequest(
                                                prepared.referenceType()
                                                        .methodsByName(boundary)
                                                        .getFirst()
                                                        .location());
                        entry.setSuspendPolicy(EventRequest.SUSPEND_EVENT_THREAD);
                        entry.enable();
                        preparation.disable();
                    } else if (event instanceof BreakpointEvent hit
                            && hit.location()
                                    .declaringType()
                                    .name()
                                    .equals(ZLinkNettyTransportConnection.class.getName())) {
                        transportAttempts++;
                    } else if (event instanceof BreakpointEvent hit
                            && (!scenario.equals("connect")
                                    || hit.thread().name().equals(LATE_CONNECT_THREAD))) {
                        paused = events;
                    }
                }
                if (paused == null) {
                    events.resume();
                }
            }
            assertTrue(paused != null, "racing path did not reach its boundary: " + output);
            entry.disable();
            process.getOutputStream()
                    .write("proceed\n".getBytes(java.nio.charset.StandardCharsets.UTF_8));
            process.getOutputStream().flush();
            assertTrue(checkpoint.await(WAIT_SECONDS, TimeUnit.SECONDS), output.toString());
            paused.resume();
            // Non-suspending transport breakpoints count attempts, including an attempt
            // whose socket the server has not accepted yet. Drain through VM disconnect.
            boolean disconnected = false;
            while (!disconnected) {
                try {
                    var events = vm.eventQueue().remove(TimeUnit.SECONDS.toMillis(WAIT_SECONDS));
                    assertTrue(events != null, "probe did not finish: " + output);
                    for (var event : events) {
                        if (event instanceof BreakpointEvent hit
                                && hit.location()
                                        .declaringType()
                                        .name()
                                        .equals(ZLinkNettyTransportConnection.class.getName())) {
                            transportAttempts++;
                        } else if (event instanceof com.sun.jdi.event.VMDisconnectEvent) {
                            disconnected = true;
                        }
                    }
                    events.resume();
                } catch (com.sun.jdi.VMDisconnectedException ended) {
                    disconnected = true;
                }
            }
            assertTrue(process.waitFor(WAIT_SECONDS, TimeUnit.SECONDS), "probe did not finish");
            assertEquals(1, transportAttempts, "transport connect attempts");
            reader.join(TimeUnit.SECONDS.toMillis(WAIT_SECONDS));
            String errors =
                    new String(
                            process.getErrorStream().readAllBytes(),
                            java.nio.charset.StandardCharsets.UTF_8);
            assertEquals(0, process.exitValue(), output + errors);
        } finally {
            // Detaching releases suspended event threads. EOF releases a probe still
            // waiting for permission, so its connector/server finally blocks can run.
            try {
                vm.dispose();
            } catch (com.sun.jdi.VMDisconnectedException disconnected) {
                // A completed probe has already disconnected the debugger.
            }
            process.getOutputStream().close();
            process.waitFor();
            reader.join();
        }
    }

    private static String testClasspath() throws Exception {
        var paths = new LinkedHashSet<String>();
        paths.addAll(
                java.util.List.of(System.getProperty("java.class.path").split(File.pathSeparator)));
        for (ClassLoader loader = ZLinkStreamLifecycleRaceTest.class.getClassLoader();
                loader != null;
                loader = loader.getParent()) {
            if (loader instanceof URLClassLoader urls) {
                for (var url : urls.getURLs()) {
                    paths.add(Path.of(url.toURI()).toString());
                }
            }
        }
        return String.join(File.pathSeparator, paths);
    }

    public static final class Probe {
        public static void main(String[] args) throws Exception {
            String scenario = args[0];
            // Install the transport breakpoint before the racing caller can open a socket.
            Class.forName(ZLinkNettyTransportConnection.class.getName());
            try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
                var options =
                        new ZLinkStreamConnectorOptions(
                                server.endpoint(),
                                ZLinkStreamDispatchMode.IMMEDIATE,
                                Duration.ofSeconds(5),
                                1,
                                Duration.ofSeconds(5),
                                64 * 1024,
                                false,
                                Duration.ofSeconds(1),
                                Duration.ofSeconds(5),
                                false,
                                Duration.ofMillis(250),
                                Duration.ofSeconds(5),
                                2.0);
                ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(options);
                try {
                    CompletableFuture<Void> racing = new CompletableFuture<>();
                    if (scenario.equals("connect")) {
                        Thread.ofPlatform()
                                .name(LATE_CONNECT_THREAD)
                                .start(
                                        () -> {
                                            try {
                                                connector
                                                        .connect()
                                                        .submit()
                                                        .toCompletableFuture()
                                                        .join();
                                                racing.complete(null);
                                            } catch (Throwable failure) {
                                                racing.completeExceptionally(failure);
                                            }
                                        });
                    } else {
                        connector
                                .connect()
                                .submit()
                                .toCompletableFuture()
                                .get(WAIT_SECONDS, TimeUnit.SECONDS);
                        connector.onDisconnected(
                                event -> {
                                    racing.complete(null);
                                    return CompletableFuture.completedFuture(null);
                                });
                        assertFalse(server.hasAdditionalConnection(Duration.ZERO));
                        if (scenario.equals("receiveFailure")) {
                            server.closeCurrentSocket();
                        } else {
                            server.sendAsync(
                                            new ZLinkStreamWireProtocol.Header(
                                                    ZLinkStreamWireProtocol.KIND_CONTROL,
                                                    ZLinkStreamWireProtocol.CODEC_RAW,
                                                    0,
                                                    null,
                                                    ZLinkSessionClosingControl.NAME,
                                                    Map.of(),
                                                    null),
                                            new byte[] {1, 4, 0, 0})
                                    .get(WAIT_SECONDS, TimeUnit.SECONDS);
                        }
                    }
                    assertEquals(
                            "proceed",
                            new BufferedReader(new InputStreamReader(System.in)).readLine());
                    if (scenario.equals("connect")) {
                        connector
                                .connect()
                                .submit()
                                .toCompletableFuture()
                                .get(WAIT_SECONDS, TimeUnit.SECONDS);
                        assertEquals(ZLinkStreamConnectionState.CONNECTED, connector.state());
                    } else {
                        connector
                                .close()
                                .submit()
                                .toCompletableFuture()
                                .get(WAIT_SECONDS, TimeUnit.SECONDS);
                        assertEquals(ZLinkStreamConnectionState.CLOSED, connector.state());
                    }
                    System.out.println("CHECKPOINT");
                    racing.get(WAIT_SECONDS, TimeUnit.SECONDS);
                    if (scenario.equals("connect")) {
                        assertTrue(connector.isConnected());
                        assertFalse(server.hasAdditionalConnection(Duration.ZERO));
                    } else {
                        assertEquals(ZLinkStreamConnectionState.CLOSED, connector.state());
                        ZLinkStreamException failure =
                                assertThrows(
                                        ZLinkStreamException.class,
                                        () -> ConnectorTestAwait.await(connector.connect()));
                        assertEquals(ZLinkStreamErrorCode.DISCONNECTED, failure.errorCode());
                    }
                } finally {
                    connector
                            .close()
                            .submit()
                            .toCompletableFuture()
                            .get(WAIT_SECONDS, TimeUnit.SECONDS);
                }
            }
        }
    }
}
