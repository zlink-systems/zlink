package systems.zlink.framework.runtime.locations;

import java.util.Objects;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;

/** Internal holder for the single complete Location Store capability. */
public final class ZLinkRegisteredLocationStores {
    private final ZLinkLocationRepository store;

    private ZLinkRegisteredLocationStores(ZLinkLocationRepository store) {
        this.store = Objects.requireNonNull(store, "store");
    }

    public static ZLinkRegisteredLocationStores fromUnified(
        ZLinkLocationRepository store) {
        return new ZLinkRegisteredLocationStores(store);
    }

    public void addTo(ZLinkHandlerActivator.MutableServices services) {
        services.add(ZLinkLocationRepository.class, store);
    }

    public ZLinkLocationRepository unifiedStore() { return store; }
    public ZLinkLocationRepository peerStore() { return store; }
    public ZLinkLocationRepository spotStore() { return store; }
    public ZLinkLocationRepository actorStore() { return store; }
    public ZLinkLocationRepository routeStore() { return store; }
    public ZLinkLocationRepository ownerLeaseStore() { return store; }
    public ZLinkLocationRepository authorityStore() { return store; }
    public ZLinkLocationRepository clientServerStore() { return store; }
    public ZLinkLocationRepository fanoutStore() { return store; }
}
