package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.net.ServerSocket;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Flow;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;
import org.springframework.context.annotation.Configuration;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.handlers.ZLinkPacket;
import systems.zlink.framework.monitoring.ZLinkClientServerRole;
import systems.zlink.framework.monitoring.ZLinkClientServerRuntime;
import systems.zlink.framework.monitoring.ZLinkClientServerStatus;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkTopologyState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;

final class ZLinkClientServerReadinessTest {
    @ParameterizedTest
    @ValueSource(ints = {100, 0})
    void serverOnlyReadinessCountsTheLocalReadyServer(int weight) {
        try (var context = start(options -> options.addClientServerChannel("work")
                .server().listen().setWeight(weight)
                .addRequestHandler(EchoHandler.class, Probe.class, Probe.class))) {
            var runtime = context.getBean(ZLinkFrameworkRuntime.class);
            var monitoring = context.getBean(ZLinkClientServerRuntime.class);
            var status = monitoring.snapshot("work");
            boolean ready = weight > 0;

            assertEquals(ZLinkFrameworkRuntimeState.SERVING, runtime.status().state());
            assertFalse(runtime.listenerStatus(ZLinkListenerKind.CLIENT_SERVER, "work")
                .endpoint().endsWith(":0"));
            assertEquals(ZLinkClientServerRole.SERVER, status.localRole());
            assertEquals(ready ? ZLinkTopologyState.READY : ZLinkTopologyState.DEGRADED,
                status.state());
            assertEquals(ready, status.isReady());
            assertEquals(ready, monitoring.isReady("work"));
            assertEquals(ready ? 1 : 0, status.readyTargetCount());
            assertEquals(1, status.targets().size());
            assertEquals(weight, status.targets().getFirst().weight());
            assertEquals(ZLinkPeerState.READY, status.targets().getFirst().state());
        }
    }

    @Test
    void clientOnlyWithoutAReadyServerIsDegraded() throws Exception {
        try (var unusedPort = new ServerSocket(0);
             var context = start(options -> options.addClientServerChannel("work")
                 .client().connect("tcp://127.0.0.1:" + unusedPort.getLocalPort()))) {
            var runtime = context.getBean(ZLinkFrameworkRuntime.class);
            var monitoring = context.getBean(ZLinkClientServerRuntime.class);
            var status = monitoring.snapshot("work");

            assertEquals(ZLinkFrameworkRuntimeState.SERVING, runtime.status().state());
            assertEquals(ZLinkClientServerRole.CLIENT, status.localRole());
            assertEquals(ZLinkTopologyState.DEGRADED, status.state());
            assertFalse(status.isReady());
            assertFalse(monitoring.isReady("work"));
            assertEquals(0, status.readyTargetCount());
        }
    }

    @ParameterizedTest
    @ValueSource(ints = {100, 0})
    void clientAndServerCountsTheLocalServerOnce(int weight) throws Exception {
        try (var context = start(options -> {
            var channel = options.addClientServerChannel("work");
            channel.client();
            channel.server().listen().setWeight(weight)
                .addRequestHandler(EchoHandler.class, Probe.class, Probe.class);
        })) {
            var runtime = context.getBean(ZLinkFrameworkRuntime.class);
            var monitoring = context.getBean(ZLinkClientServerRuntime.class);
            var status = monitoring.snapshot("work");
            boolean ready = weight > 0;

            assertEquals(ZLinkFrameworkRuntimeState.SERVING, runtime.status().state());
            assertEquals(ZLinkClientServerRole.CLIENT_AND_SERVER, status.localRole());
            assertEquals(ready ? ZLinkTopologyState.READY : ZLinkTopologyState.DEGRADED,
                status.state());
            assertEquals(ready, status.isReady());
            assertEquals(ready, monitoring.isReady("work"));
            assertEquals(ready ? 1 : 0, status.readyTargetCount());
            var observed = observeStatus(monitoring);
            assertEquals(status.localRole(), observed.localRole());
            assertEquals(status.state(), observed.state());
            assertEquals(ready, observed.isReady());
            assertEquals(ready ? 1 : 0, observed.readyTargetCount());
        }
    }

    @Test
    void localReadinessUsesTheCurrentServerWeight() {
        try (var context = start(options -> options.addClientServerChannel("work")
                .server().listen().setWeight(100)
                .addRequestHandler(EchoHandler.class, Probe.class, Probe.class))) {
            var runtime = context.getBean(ZLinkFrameworkRuntime.class);
            var monitoring = runtime.clientServerRuntime();
            var socketOptions = runtime.channelRuntimeOptions()
                .clientServerChannel("work").configureServerSocket();

            assertTrue(monitoring.isReady("work"));
            socketOptions.weight(0);
            var disabled = monitoring.snapshot("work");
            assertFalse(disabled.isReady());
            assertEquals(ZLinkTopologyState.DEGRADED, disabled.state());
            assertEquals(0, disabled.readyTargetCount());
            assertEquals(0, disabled.targets().getFirst().weight());

            socketOptions.weight(100);
            var enabled = monitoring.snapshot("work");
            assertTrue(enabled.isReady());
            assertEquals(1, enabled.readyTargetCount());
            assertEquals(100, enabled.targets().getFirst().weight());
        }
    }

    @Test
    void readyServerOnlyStillRejectsOutboundCallsWithoutTheClientRole() {
        try (var context = start(options -> options.addClientServerChannel("work")
                .server().listen().setWeight(100)
                .addRequestHandler(EchoHandler.class, Probe.class, Probe.class))) {
            var runtime = context.getBean(ZLinkFrameworkRuntime.class);

            assertTrue(runtime.clientServerRuntime().isReady("work"));
            assertEquals(ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                assertThrows(ZLinkConfigurationException.class,
                    () -> runtime.client().sendToChannel("work", new Probe("send")))
                    .kind());
            assertEquals(ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                assertThrows(ZLinkConfigurationException.class,
                    () -> runtime.client().requestToChannel("work", new Probe("request")))
                    .kind());
        }
    }

    private static AnnotationConfigApplicationContext start(ZLinkFrameworkConfigurer configure) {
        var context = new AnnotationConfigApplicationContext();
        context.registerBean(ZLinkFrameworkConfigurer.class, () -> options -> {
            options.configureNetwork().setBindHost("127.0.0.1");
            options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            configure.configure(options);
        });
        context.register(EnabledFramework.class);
        context.refresh();
        return context;
    }

    private static ZLinkClientServerStatus observeStatus(
        ZLinkClientServerRuntime monitoring) throws Exception {
        var ready = new CompletableFuture<ZLinkClientServerStatus>();
        var subscription = new AtomicReference<Flow.Subscription>();
        monitoring.observe("work", 16).subscribe(new Flow.Subscriber<>() {
            @Override
            public void onSubscribe(Flow.Subscription value) {
                subscription.set(value);
                value.request(Long.MAX_VALUE);
            }

            @Override
            public void onNext(ZLinkObservedStatus<ZLinkClientServerStatus> observed) {
                ready.complete(observed.status());
            }

            @Override
            public void onError(Throwable failure) {
                ready.completeExceptionally(failure);
            }

            @Override
            public void onComplete() {
                ready.completeExceptionally(new AssertionError("monitoring stopped before readiness"));
            }
        });
        try {
            return ready.get(5, TimeUnit.SECONDS);
        } finally {
            if (subscription.get() != null) {
                subscription.get().cancel();
            }
        }
    }

    @ZLinkPacket("ReadinessProbe")
    public record Probe(String value) { }

    public static final class EchoHandler implements ZLinkRequestHandler<Probe, Probe> {
        @Override
        public CompletionStage<Probe> handle(Probe request, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture(request);
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class EnabledFramework { }
}
