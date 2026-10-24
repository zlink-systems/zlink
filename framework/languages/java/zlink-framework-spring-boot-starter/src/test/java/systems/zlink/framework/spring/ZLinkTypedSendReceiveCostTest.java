package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.*;

import com.google.protobuf.ByteString;
import com.google.protobuf.BytesValue;
import com.google.protobuf.CodedOutputStream;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteSendHandler;
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

import java.net.URLClassLoader;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;

/** Exact allocations on the public typed send path, with TLAB and escape analysis disabled. */
public final class ZLinkTypedSendReceiveCostTest {
    private static final int SENDS = 1000;
    private static final RoutingId TARGET = RoutingId.from("typed-cost-target");
    private static final BytesValue PAYLOAD = BytesValue.of(ByteString.copyFrom(new byte[1024]));

    @Test
    void typedSendDoesNotRepeatHeaderViewsOrConstructorIteration() throws Exception {
        var classpath = new LinkedHashSet<String>();
        for (ClassLoader loader = getClass().getClassLoader();
                loader != null;
                loader = loader.getParent()) {
            if (loader instanceof URLClassLoader urls)
                for (var url : urls.getURLs()) classpath.add(Path.of(url.toURI()).toString());
        }
        classpath.addAll(
                List.of(System.getProperty("java.class.path").split(java.io.File.pathSeparator)));
        Path output = Files.createTempFile("typed-send-cost-", ".log");
        try {
            var process =
                    new ProcessBuilder(
                                    Path.of(System.getProperty("java.home"), "bin", "java")
                                            .toString(),
                                    "--enable-native-access=ALL-UNNAMED",
                                    "-XX:-UseTLAB",
                                    "-XX:-DoEscapeAnalysis",
                                    "-XX:ActiveProcessorCount=2",
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

    public static void main(String[] args) throws Exception {
        var sink = new Sink();
        Path path = Files.createTempFile("typed-send-cost-", ".jfr");
        try (var target = host(sink, null);
                var source =
                        host(
                                null,
                                target.getBean(ZLinkFrameworkRuntime.class)
                                        .listenerStatus(ZLinkListenerKind.ROUTE_MESH, "cost")
                                        .endpoint())) {
            awaitReady(source.getBean(ZLinkRouteMeshRuntime.class));
            var client = source.getBean(ZLinkRouteClient.class);
            send(client, sink, 200);
            bindingRoundTrip(200);
            try (var recording = new Recording()) {
                recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                recording.enable("jdk.DataLoss");
                recording.start();
                send(client, sink, SENDS);
                bindingRoundTrip(SENDS);
                recording.stop();
                recording.dump(path);
            }
            long headers = 0,
                    views = 0,
                    constructorIterators = 0,
                    encoders = 0,
                    messages = 0,
                    payloadDecodes = 0,
                    activations = 0;
            long baselineSendBytes = 0,
                    baselineSendObjects = 0,
                    baselineReceiveBytes = 0,
                    baselineReceiveObjects = 0;
            long nativeAddresses = 0,
                    baselineAddresses = 0,
                    baselineEncoders = 0,
                    baselineMessages = 0;
            long targetBytes = 0,
                    targetObjects = 0,
                    callerBytes = 0,
                    callerObjects = 0,
                    instanceOwners = 0,
                    planCallbacks = 0;
            var classes = new TreeMap<String, Long>();
            try (var events = new RecordingFile(path)) {
                while (events.hasMoreEvents()) {
                    var e = events.readEvent();
                    assertNotEquals("jdk.DataLoss", e.getEventType().getName());
                    if (!e.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB")
                            || e.getStackTrace() == null) continue;
                    String type = e.getClass("objectClass").getName();
                    boolean decode = false,
                            activation = false,
                            serialize = false,
                            targetPath = false,
                            baselineSend = false,
                            baselineReceive = false,
                            address = false,
                            planLookup = false;
                    for (var frame : e.getStackTrace().getFrames()) {
                        String owner = frame.getMethod().getType().getName();
                        String method = frame.getMethod().getName();
                        baselineSend |=
                                owner.equals(ZLinkTypedSendReceiveCostTest.class.getName())
                                        && method.equals("bindingPayload");
                        baselineReceive |=
                                owner.equals(ZLinkTypedSendReceiveCostTest.class.getName())
                                        && method.equals("bindingReceive");
                        address |=
                                owner.equals("systems.zlink.runtime.nativeapi.NativeMessage")
                                        && method.equals("messageData");
                        decode |=
                                owner.endsWith(".ZLinkChannelEnvelope")
                                        && method.equals("decodeDispatchHeader");
                        planLookup |=
                                owner.endsWith(".ZLinkSpringHandlerFactory")
                                        && method.equals("handlerPlan");
                        activation |=
                                owner.endsWith(".ZLinkSpringHandlerFactory$SpringActivation")
                                        && method.equals("create");
                        serialize |=
                                owner.endsWith(".ZLinkProtobufMessageSerializer")
                                        && method.equals("serializeOwned");
                        targetPath |=
                                owner.endsWith(".ZLinkMeshApplicationDispatcher")
                                        || owner.endsWith(".ZLinkJavaRawServicePort")
                                                && method.equals("receiveOnLane");
                    }
                    if (baselineSend) {
                        baselineSendObjects++;
                        baselineSendBytes += e.getLong("allocationSize");
                    }
                    if (baselineReceive) {
                        baselineReceiveObjects++;
                        baselineReceiveBytes += e.getLong("allocationSize");
                    }
                    if (address && type.equals("jdk.internal.foreign.NativeMemorySegmentImpl")) {
                        if (serialize) nativeAddresses++;
                        if (baselineSend) baselineAddresses++;
                    }
                    if (baselineSend && type.startsWith("com.google.protobuf.CodedOutputStream$"))
                        baselineEncoders++;
                    if (baselineSend && type.equals("systems.zlink.contracts.messaging.Message"))
                        baselineMessages++;
                    if (targetPath && type.equals(BytesValue.class.getName())) payloadDecodes++;
                    if (targetPath && type.endsWith(".ZLinkSpringHandlerFactory$SpringActivation"))
                        activations++;
                    if (targetPath && type.endsWith(".ZLinkHandlerInstanceOwner")) instanceOwners++;
                    if (planLookup
                            && type.startsWith(
                                    ZLinkSpringHandlerFactory.class.getName() + "$$Lambda"))
                        planCallbacks++;
                    if (targetPath) {
                        targetObjects++;
                        targetBytes += e.getLong("allocationSize");
                        classes.merge(type, 1L, Long::sum);
                    }
                    if (serialize) {
                        callerObjects++;
                        callerBytes += e.getLong("allocationSize");
                    }
                    if (decode && type.endsWith("ZLinkChannelEnvelope$Header")) headers++;
                    if (decode && type.startsWith("java.nio.") && type.contains("ByteBuffer"))
                        views++;
                    if (activation && type.equals(List.of(0).iterator().getClass().getName()))
                        constructorIterators++;
                    if (serialize && type.startsWith("com.google.protobuf.CodedOutputStream$"))
                        encoders++;
                    if (serialize && type.equals("systems.zlink.contracts.messaging.Message"))
                        messages++;
                }
            }
            System.out.printf(
                    "sends=%d headerDecodes=%d headerViews=%d constructorIterators=%d protobufEncoders=%d codecMessages=%d targetObjects=%d targetBytes=%d callerCodecObjects=%d callerCodecBytes=%d%n",
                    SENDS,
                    headers,
                    views,
                    constructorIterators,
                    encoders,
                    messages,
                    targetObjects,
                    targetBytes,
                    callerObjects,
                    callerBytes);
            System.out.printf(
                    "baselineSendObjects=%d baselineSendBytes=%d baselineReceiveObjects=%d baselineReceiveBytes=%d nativePayloadAddresses=%d baselinePayloadAddresses=%d baselineEncoders=%d baselineMessages=%d%n",
                    baselineSendObjects,
                    baselineSendBytes,
                    baselineReceiveObjects,
                    baselineReceiveBytes,
                    nativeAddresses,
                    baselineAddresses,
                    baselineEncoders,
                    baselineMessages);
            assertEquals(SENDS, baselineEncoders);
            assertEquals(SENDS, baselineMessages);
            assertTrue(
                    baselineAddresses >= SENDS,
                    "record real nonempty FFM address access in the binding control");
            assertEquals(
                    baselineAddresses,
                    nativeAddresses,
                    "codec adds no native payload address read");
            classes.forEach(
                    (type, count) -> System.out.printf("targetAllocation %s %d%n", type, count));
            System.out.printf(
                    "payloadDecodes=%d springActivations=%d%n", payloadDecodes, activations);
            assertEquals(SENDS, payloadDecodes, "one typed payload decode per accepted send");
            assertEquals(SENDS, activations, "one Spring dependency scope per dispatch");
            System.out.printf("unfilteredInstanceOwners=%d%n", instanceOwners);
            assertEquals(
                    0,
                    instanceOwners,
                    "a single unfiltered handler needs only its activation scope");
            System.out.printf("springPlanCallbacks=%d%n", planCallbacks);
            assertEquals(
                    0, planCallbacks, "registration-time factory is already the plan callback");
            assertEquals(SENDS, headers, "one header decode per accepted send");
            assertEquals(SENDS, encoders, "one codec writer per payload");
            assertEquals(SENDS, messages, "one owned codec Message per payload");
            assertEquals(SENDS + 200, sink.created, "dispatch owns a fresh handler");
            assertEquals(sink.created, sink.closed, "dispatch releases each handler once");
            assertEquals(SENDS, views, "probe and decode share one borrowed header view");
            assertEquals(
                    0,
                    constructorIterators,
                    "prepared constructor traversal needs no cursor object");

        } finally {
            Files.deleteIfExists(path);
        }
    }

    private static void bindingRoundTrip(int count) throws Exception {
        try (var context = Zlink.createContext();
                var source = context.createRouterSocket();
                var target = context.createRouterSocket();
                var received = new Received()) {
            source.setRoutingId(RoutingId.from("binding-cost-source"));
            target.setRoutingId(TARGET);
            String endpoint = "inproc://binding-cost-" + UUID.randomUUID();
            target.bind(endpoint);
            source.connect(endpoint);
            for (int i = 0; i < count; i++) {
                bindingSend(source);
                bindingReceive(target, received);
            }
        }
    }

    private static void bindingSend(RouterSocket source) throws Exception {
        try (var encoded = bindingPayload()) {
            source.send(TARGET).message(encoded).submit_sync();
        }
    }

    private static Message bindingPayload() throws Exception {
        var encoded = Message.allocate(PAYLOAD.getSerializedSize());
        try {
            var output = CodedOutputStream.newInstance(encoded.mutableDataBuffer());
            PAYLOAD.writeTo(output);
            output.flush();
            output.checkNoSpaceLeft();
            return encoded;
        } catch (Exception failure) {
            encoded.close();
            throw failure;
        }
    }

    private static void bindingReceive(RouterSocket target, Received received) throws Exception {
        assertTrue(target.recv(received, RecvFlags.NONE));
        assertEquals(PAYLOAD, BytesValue.parseFrom(received.parts().getFirst().dataBuffer()));
    }

    private static AnnotationConfigApplicationContext host(Sink sink, String endpoint) {
        var context = new AnnotationConfigApplicationContext();
        if (sink != null) context.registerBean(Sink.class, () -> sink);
        context.registerBean(
                ZLinkFrameworkConfigurer.class,
                () ->
                        options -> {
                            options.configureNetwork().setBindHost("127.0.0.1");
                            options.codecs().use(ZLinkProtobufCodec.defaultCodec());
                            var mesh =
                                    options.addRouteMesh("cost")
                                            .listen("tcp://127.0.0.1:0")
                                            .setRoutingId(
                                                    sink == null
                                                            ? RoutingId.from("typed-cost-source")
                                                            : TARGET);
                            if (sink != null)
                                mesh.addRouteSendHandler(Handler.class, BytesValue.class);
                            else mesh.peerConnections().connect(TARGET, endpoint);
                        });
        context.register(Enabled.class, ZLinkFrameworkAutoConfiguration.class);
        context.refresh();
        return context;
    }

    @EnableZLinkFramework
    static class Enabled {}

    public static final class Sink {
        final BlockingQueue<Boolean> completed = new LinkedBlockingQueue<>();
        int created;
        int closed;
    }

    public static final class Handler implements ZLinkRouteSendHandler<BytesValue>, AutoCloseable {
        private final Sink sink;

        public Handler(Sink sink) {
            this.sink = sink;
            sink.created++;
        }

        public CompletionStage<Void> handle(BytesValue message, ZLinkRouteMessageContext context) {
            assertEquals(PAYLOAD, message);
            return CompletableFuture.completedFuture(null);
        }

        public void close() {
            sink.closed++;
            sink.completed.add(true);
        }
    }

    private static void send(ZLinkRouteClient client, Sink sink, int count) throws Exception {
        for (int i = 0; i < count; i++) {
            client.sendToNode("cost", TARGET, PAYLOAD)
                    .submit()
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            assertEquals(Boolean.TRUE, sink.completed.poll(5, TimeUnit.SECONDS));
        }
    }

    private static void awaitReady(ZLinkRouteMeshRuntime runtime) throws Exception {
        var ready = new CompletableFuture<Void>();
        var subscription = new AtomicReference<Flow.Subscription>();
        runtime.observe("cost", 16)
                .subscribe(
                        new Flow.Subscriber<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>>() {
                            public void onSubscribe(Flow.Subscription value) {
                                subscription.set(value);
                                value.request(Long.MAX_VALUE);
                            }

                            public void onNext(ZLinkObservedStatus<ZLinkMeshNodeSnapshot> value) {
                                if (value.status().peers().stream()
                                        .anyMatch(
                                                p ->
                                                        p.nodeRid().equals(TARGET)
                                                                && p.state()
                                                                        == ZLinkPeerState.READY))
                                    ready.complete(null);
                            }

                            public void onError(Throwable failure) {
                                ready.completeExceptionally(failure);
                            }

                            public void onComplete() {
                                ready.completeExceptionally(
                                        new AssertionError("closed before ready"));
                            }
                        });
        try {
            ready.get(5, TimeUnit.SECONDS);
        } finally {
            subscription.get().cancel();
        }
    }
}
