package systems.zlink.framework.runtime.binding;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.util.function.Supplier;

/** Test-only constructor access for the Java mesh backend lane. */
public final class ZLinkJavaBackendAdapterFactoryTestAccess {
    private ZLinkJavaBackendAdapterFactoryTestAccess() {}

    public static ZLinkBackendAdapterProvider withRawSpotStateLaneFactory(
            Supplier<ZLinkStateLane> factory) {
        return new ZLinkJavaBackendAdapterFactory(factory);
    }
}
