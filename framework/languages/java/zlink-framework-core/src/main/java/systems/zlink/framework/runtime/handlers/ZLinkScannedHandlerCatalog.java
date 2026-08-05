package systems.zlink.framework.runtime.handlers;

import java.util.ArrayList;
import java.util.List;
import java.util.Set;

public final class ZLinkScannedHandlerCatalog {
    private final List<ZLinkScannedHandler> handlers;

    public ZLinkScannedHandlerCatalog(List<ZLinkScannedHandler> handlers) {
        this.handlers = List.copyOf(handlers);
    }

    public List<ZLinkScannedHandler> handlers() {
        return handlers;
    }

    public List<ZLinkScannedHandler> matching(
        Set<String> groups,
        ZLinkScannedHandlerSurface surface,
        ZLinkScannedHandlerKind kind) {
        if (groups.isEmpty() || handlers.isEmpty()) {
            return List.of();
        }
        List<ZLinkScannedHandler> matched = new ArrayList<>();
        for (ZLinkScannedHandler handler : handlers) {
            if (handler.surface() == surface
                && handler.kind() == kind
                && handler.groups().stream().anyMatch(groups::contains)) {
                matched.add(handler);
            }
        }
        return List.copyOf(matched);
    }

    public boolean containsGroup(String group) {
        return handlers.stream().anyMatch(handler -> handler.groups().contains(group));
    }

    public boolean groupHasOnly(
        String group,
        ZLinkScannedHandlerSurface surface,
        Set<ZLinkScannedHandlerKind> allowedKinds) {
        boolean found = false;
        for (ZLinkScannedHandler handler : handlers) {
            if (!handler.groups().contains(group)) {
                continue;
            }
            found = true;
            if (handler.surface() != surface || !allowedKinds.contains(handler.kind())) {
                return false;
            }
        }
        return found;
    }
}
