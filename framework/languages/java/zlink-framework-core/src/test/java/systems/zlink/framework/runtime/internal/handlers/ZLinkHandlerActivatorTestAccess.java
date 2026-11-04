package systems.zlink.framework.runtime.internal.handlers;

import java.util.Map;

public final class ZLinkHandlerActivatorTestAccess {
    private ZLinkHandlerActivatorTestAccess() {}

    public static Map<Class<?>, Object> serviceIndex(
            ZLinkHandlerActivator.MutableServices services) {
        return services.serviceIndex;
    }

    public static void serviceIndex(
            ZLinkHandlerActivator.MutableServices services, Map<Class<?>, Object> index) {
        services.serviceIndex = index;
    }
}
