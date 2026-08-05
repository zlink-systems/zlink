package systems.zlink.framework.runtime.locations;

import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;

public final class ZLinkLocationRegistration {
    private final ZLinkLocationOptions options = new ZLinkLocationOptions();
    private ZLinkLocationStore storeInstance;

    public ZLinkLocationOptions options() {
        return options;
    }

    public ZLinkLocationStore storeInstance() {
        return storeInstance;
    }

    public void setStoreInstance(ZLinkLocationStore storeInstance) {
        this.storeInstance = storeInstance;
    }

    public boolean enabled() {
        return storeInstance != null;
    }
}
