package systems.zlink.framework.configuration;

import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkSpotRelocationAdapter;

/**
 * Configures an Instance Spot factory.
 *
 * @param <TSpot> the Spot type created by the factory
 */
public interface ZLinkInstanceSpotFactoryBuilder<TSpot extends ZLinkInstanceSpot> {
    ZLinkInstanceSpotFactoryBuilder<TSpot> stableTypeLimit(int limit);

    void disableRelocation();

    void recreateOnRelocation();

    void preserveStateWith(
        Class<? extends ZLinkSpotRelocationAdapter<TSpot>> adapterClass);
}
