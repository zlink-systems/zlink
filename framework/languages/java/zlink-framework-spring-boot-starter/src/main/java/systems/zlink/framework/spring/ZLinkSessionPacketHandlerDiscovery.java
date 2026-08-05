package systems.zlink.framework.spring;

import static systems.zlink.framework.runtime.handlers.ZLinkHandlerInterfaceNames.KOTLIN_SESSION_PACKET_HANDLER;

import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.util.LinkedHashSet;
import java.util.Set;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.streams.StreamNodeRegistration;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

final class ZLinkSessionPacketHandlerDiscovery {
    private ZLinkSessionPacketHandlerDiscovery() {
    }

    static void discover(DefaultZLinkFrameworkOptions options) {
        for (StreamNodeRegistration streamNode : options.registration().streamNodes()) {
            Class<?> sessionType = streamNode.sessionType();
            if (sessionType == null) {
                continue;
            }
            for (Class<?> contextType : findSessionPacketDispatcherContexts(sessionType)) {
                for (String packageName : searchPackages(options, sessionType, contextType)) {
                    for (Class<?> handlerType : findSessionPacketHandlers(packageName, contextType)) {
                        streamNode.addSessionPacketHandler(handlerType);
                    }
                }
            }
        }
    }

    private static Set<String> searchPackages(
        DefaultZLinkFrameworkOptions options,
        Class<?> sessionType,
        Class<?> contextType) {
        Set<String> packageNames = new LinkedHashSet<>();
        packageNames.add(sessionType.getPackageName());
        packageNames.add(contextType.getPackageName());
        for (Class<?> markerType : options.registration().handlerPackageMarkers()) {
            packageNames.add(markerType.getPackageName());
        }
        return packageNames;
    }

    private static Set<Class<?>> findSessionPacketDispatcherContexts(Class<?> sessionType) {
        Set<Class<?>> contextTypes = new LinkedHashSet<>();
        for (var constructor : sessionType.getConstructors()) {
            for (Type parameter : constructor.getGenericParameterTypes()) {
                Class<?> contextType = sessionPacketDispatcherContextType(parameter);
                if (contextType != null) {
                    contextTypes.add(contextType);
                }
            }
        }
        return contextTypes;
    }

    private static Class<?> sessionPacketDispatcherContextType(Type parameter) {
        if (!(parameter instanceof ParameterizedType parameterized)
            || parameterized.getRawType() != ZLinkSessionPacketDispatcher.class) {
            return null;
        }
        Type argument = parameterized.getActualTypeArguments()[0];
        return argument instanceof Class<?> contextType ? contextType : null;
    }

    private static Set<Class<?>> findSessionPacketHandlers(String packageName, Class<?> contextType) {
        Set<Class<?>> handlers = new LinkedHashSet<>();
        for (Class<?> type : ZLinkClasspathTypeScanner.findCandidateTypes(
            packageName,
            ZLinkTypedSessionPacketHandler.class)) {
            if (sessionPacketHandlerContextType(type) == contextType) {
                handlers.add(type);
            }
        }
        return handlers;
    }

    private static Class<?> sessionPacketHandlerContextType(Class<?> handlerType) {
        for (Type type : handlerType.getGenericInterfaces()) {
            Class<?> contextType = sessionPacketHandlerContextType(type);
            if (contextType != null) {
                return contextType;
            }
        }
        Class<?> superclass = handlerType.getSuperclass();
        return superclass == null ? null : sessionPacketHandlerContextType(superclass);
    }

    private static Class<?> sessionPacketHandlerContextType(Type type) {
        if (!(type instanceof ParameterizedType parameterized)
            || !(parameterized.getRawType() instanceof Class<?> rawType)
            || (!ZLinkTypedSessionPacketHandler.class.isAssignableFrom(rawType)
                && !KOTLIN_SESSION_PACKET_HANDLER.equals(rawType.getName()))) {
            return null;
        }
        Type argument = parameterized.getActualTypeArguments()[0];
        return argument instanceof Class<?> contextType ? contextType : null;
    }
}
