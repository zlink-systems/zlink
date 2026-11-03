package systems.zlink.framework.runtime.host;

import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.locations.ZLinkLocationRuntime;
import systems.zlink.framework.runtime.locations.ZLinkStatefulAuthorityRouteRuntime;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;

import java.util.concurrent.CompletionStage;

/** Test-only access to the package-owned runtime bootstrap. */
public final class ZLinkFrameworkRuntimeTestAccess {
    private ZLinkFrameworkRuntimeTestAccess() {}

    public static ZLinkFrameworkRuntime start(DefaultZLinkFrameworkOptions options) {
        return ZLinkFrameworkRuntime.startHost(
                options,
                new ZLinkJavaBackendAdapterFactory(),
                ZLinkHandlerActivator.reflection(),
                null);
    }

    public static ZLinkFrameworkRuntime start(
            DefaultZLinkFrameworkOptions options, ZLinkBackendAdapterProvider backendProvider) {
        return ZLinkFrameworkRuntime.start(options, backendProvider);
    }

    public static ZLinkFrameworkRuntime startHost(
            DefaultZLinkFrameworkOptions options, ZLinkBackendAdapterProvider backendProvider) {
        return ZLinkFrameworkRuntime.startHost(
                options, backendProvider, ZLinkHandlerActivator.reflection(), null);
    }

    public static CompletionStage<Void> startupCompletion(ZLinkFrameworkRuntime runtime) {
        return runtime.startupCompletion();
    }

    public static ZLinkChannelRuntime channels(ZLinkFrameworkRuntime runtime) {
        return runtime.channels;
    }

    public static ZLinkMeshDrainCoordinator meshDrains(ZLinkFrameworkRuntime runtime) {
        return runtime.meshDrains;
    }

    public static ZLinkStatefulAuthorityRouteRuntime authorityRouteRuntime(
            ZLinkFrameworkRuntime runtime) {
        return runtime.authorityRouteRuntime;
    }

    public static ZLinkLocationRuntime locationRuntime(ZLinkFrameworkRuntime runtime) {
        return runtime.locationRuntime;
    }

    public static Object locationAutoConnectHost(ZLinkFrameworkRuntime runtime) {
        return runtime.locationAutoConnectHost;
    }

    public static Object spotRetire(ZLinkFrameworkRuntime runtime) {
        return runtime.spotRetire;
    }

    public static ZLinkStoreLocationResolvers storeLocationResolvers(
            ZLinkFrameworkRuntime runtime) {
        return runtime.storeLocationResolvers;
    }

    public static boolean ownerAdmissionOpen(ZLinkFrameworkRuntime runtime) {
        return runtime.ownerAdmissionOpen();
    }

    /** Runs one owner lease renewal now; completes after any republication it triggers. */
    public static CompletionStage<Boolean> renewOwnerLeaseOnce(ZLinkFrameworkRuntime runtime) {
        return runtime.renewOwnerLeaseOnce();
    }

    public static ZLinkFrameworkRuntime start(
            DefaultZLinkFrameworkOptions options,
            ZLinkBackendAdapterProvider backendProvider,
            ZLinkHandlerActivator handlerActivator,
            ZLinkRuntimeEventDispatcher eventDispatcher) {
        return ZLinkFrameworkRuntime.start(
                options, backendProvider, handlerActivator, eventDispatcher);
    }
}
