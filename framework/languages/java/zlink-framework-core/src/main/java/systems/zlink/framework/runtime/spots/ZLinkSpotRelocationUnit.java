package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.configuration.ZLinkObjectFactoryRegistration.RelocationPolicy;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;

import java.time.Instant;
import java.util.concurrent.CompletionStage;

record ZLinkSpotRelocationUnit(
        String id,
        String mesh,
        ZLinkPlacementObjectKind kind,
        String stableType,
        Class<?> spotType,
        RelocationPolicy policy,
        Source source) {
    SpotActivationBase<?> activation() {
        return source.activation();
    }

    long generation() {
        return activation().backendSpot.lifecycleGeneration();
    }

    interface Source {
        SpotActivationBase<?> activation();

        ZLinkUserSpotRelocationBarrier relocationBarrier(ZLinkActorSessionCoordinator actors);

        default boolean matchesAuthorityFence(ZLinkAuthoritySnapshot snapshot) {
            return false;
        }

        CompletionStage<Void> cleanupRelocationSource(
                ZLinkSpotLifecycle spots,
                ZLinkUserSpotRelocationBarrier barrier,
                long generation,
                Instant deadline);
    }
}
