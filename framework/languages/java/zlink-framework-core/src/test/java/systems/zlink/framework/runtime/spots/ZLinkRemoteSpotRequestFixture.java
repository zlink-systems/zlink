package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkRequestStage;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.spots.ZLinkSpotCreateState;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Function;
import java.util.function.UnaryOperator;

final class ZLinkRemoteSpotRequestFixture implements AutoCloseable {
    static final String MESH = "first-request";
    static final String SPOT = "first-request-spot";
    static final RoutingId REQUESTER_RID = RoutingId.from("requester");

    private final ZLinkFrameworkRuntime owner;
    final ZLinkFrameworkRuntime requester;

    ZLinkRemoteSpotRequestFixture(
            UnaryOperator<ZLinkLocationRepository> ownerStore,
            UnaryOperator<ZLinkLocationRepository> requesterStore)
            throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var monotonicNanos = new AtomicLong();
        var fencingMargin = Duration.ofNanos(1);
        var repository =
                new ZLinkProviderLocationRepository(locations, monotonicNanos::get, fencingMargin);
        owner = start(locations, ownerStore.apply(repository), RoutingId.from("owner"), true);
        try {
            awaitServing(owner, monotonicNanos, 3);
            var created =
                    await(owner.spotManager().getOrCreate(SPOT, "echo").inMesh(MESH).submit());
            requester = start(locations, requesterStore.apply(repository), REQUESTER_RID, false);
            try {
                awaitServing(requester, monotonicNanos, 6);
                var existing =
                        await(
                                requester
                                        .spotManager()
                                        .getOrCreate(SPOT, "echo")
                                        .inMesh(MESH)
                                        .submit());
                assertEquals(ZLinkSpotCreateState.EXISTING, existing.state());
                assertEquals(created.spot(), existing.spot());
            } catch (Exception | Error failure) {
                requester.close();
                throw failure;
            }
        } catch (Exception | Error failure) {
            owner.close();
            throw failure;
        }
        ZLinkRemoteSpotFirstRequestTest.CALLS.set(0);
    }

    ZLinkRequestStage<String> request(String value, Duration timeout) {
        return requester
                .route()
                .requestToSpot(SPOT, new ZLinkRemoteSpotFirstRequestTest.Probe(value))
                .inMesh(MESH)
                .timeout(timeout)
                .submit(String.class);
    }

    private static ZLinkFrameworkRuntime start(
            ZLinkInMemoryLocationStore store,
            ZLinkLocationRepository repository,
            RoutingId rid,
            boolean server) {
        var options = new DefaultZLinkFrameworkOptions();
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        options.addLocationStore(store);
        var mesh = options.addRouteMesh(MESH).setRoutingId(rid).listen("tcp://127.0.0.1:0");
        if (server) {
            mesh.objects()
                    .server()
                    .addSpotFactory(
                            "echo",
                            ZLinkRemoteSpotFirstRequestTest.Echo.class,
                            factory -> factory.disableRelocation());
        } else {
            mesh.objects().client();
        }
        return ZLinkFrameworkRuntimeTestAccess.startWithConstruction(
                options,
                new ZLinkJavaBackendAdapterFactory(),
                ZLinkHandlerActivator.reflection(),
                null,
                new ZLinkFrameworkRuntimeTestAccess.Construction()
                        .locationStores(ZLinkRegisteredLocationStores.fromUnified(repository)));
    }

    private static void awaitServing(
            ZLinkFrameworkRuntime runtime, AtomicLong monotonicNanos, long nanos) throws Exception {
        await(ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime));
        monotonicNanos.set(nanos);
        assertTrue(await(ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(runtime)));
        assertTrue(runtime.isReady());
    }

    static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(5, TimeUnit.SECONDS);
    }

    static ZLinkLocationRepository intercept(
            ZLinkLocationRepository store, String operation, Function<Object[], ?> substitute) {
        var type = ZLinkLocationRepository.class;
        return type.cast(
                Proxy.newProxyInstance(
                        type.getClassLoader(),
                        new Class<?>[] {type},
                        (proxy, method, args) -> {
                            if (method.getName().equals(operation)) {
                                var result = substitute.apply(args);
                                if (result != null) return result;
                            }
                            try {
                                return method.invoke(store, args);
                            } catch (InvocationTargetException failure) {
                                throw failure.getCause();
                            }
                        }));
    }

    @Override
    public void close() {
        try (owner;
                requester) {
            // The requester closes before the owner.
        }
    }
}
