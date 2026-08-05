package systems.zlink.framework.runtime.handlers;

import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

public final class ZLinkHandlerScanner {
    private ZLinkHandlerScanner() {
    }

    public static ZLinkScannedHandlerCatalog scan(Set<Class<?>> markerTypes) {
        if (markerTypes.isEmpty()) {
            return new ZLinkScannedHandlerCatalog(List.of());
        }

        Set<Class<?>> candidates = new LinkedHashSet<>();
        for (Class<?> markerType : markerTypes) {
            candidates.addAll(ZLinkHandlerPackageScanner.scan(markerType));
        }

        List<ZLinkScannedHandler> handlers = new ArrayList<>();
        for (Class<?> candidate : candidates) {
            if (!isConcrete(candidate)) {
                continue;
            }
            Set<String> groups = resolveGroups(candidate);
            ZLinkAnnotationHandlerScanner.addHandlers(handlers, candidate, groups);
            ZLinkInterfaceHandlerScanner.addHandlers(handlers, candidate, groups);
        }
        return new ZLinkScannedHandlerCatalog(handlers);
    }

    private static boolean isConcrete(Class<?> type) {
        int modifiers = type.getModifiers();
        return !type.isInterface()
            && !type.isAnnotation()
            && !type.isEnum()
            && !Modifier.isAbstract(modifiers);
    }

    private static Set<String> resolveGroups(Class<?> type) {
        ZLinkHandlerGroup[] groups = type.getAnnotationsByType(ZLinkHandlerGroup.class);
        if (groups.length == 0) {
            return Set.of();
        }
        Set<String> resolved = new LinkedHashSet<>();
        for (ZLinkHandlerGroup group : groups) {
            if (!group.value().isBlank()) {
                resolved.add(group.value());
            }
        }
        return Set.copyOf(resolved);
    }
}
