package systems.zlink.framework.configuration;

import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotRelocationAdapter;

/**
 * Configures a User Spot factory.
 *
 * @param <TSpot> the Spot type created by the factory
 */
public interface ZLinkUserSpotFactoryBuilder<TSpot extends ZLinkSpot<?>> {
    ZLinkUserSpotFactoryBuilder<TSpot> stableTypeLimit(int limit);

    ZLinkUserSpotFactoryBuilder<TSpot> executionMode(
        ZLinkUserSpotExecutionMode mode);

    ZLinkUserSpotFactoryBuilder<TSpot> relocationReadiness(
        ZLinkSpotRelocationReadinessMode mode);

    void disableRelocation();

    void recreateOnRelocation();

    void preserveStateWith(
        Class<? extends ZLinkSpotRelocationAdapter<TSpot>> adapterClass);
}
