package systems.zlink.framework.runtime.internal.spots;

import java.util.Optional;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.SpotHandle;

public interface SpotTransportAddressResolver {
    default CompletionStage<Optional<SpotTransportAddress>> resolve(SpotHandle handle) {
        return resolve(handle.spotId());
    }

    CompletionStage<Optional<SpotTransportAddress>> resolve(String spotId);

    /**
     * Removes a cached positive route after the target reports that the
     * resolved owner no longer accepts it. Custom resolvers may keep this as
     * a no-op when they do not cache routes.
     */
    default void invalidate(String spotId) {
    }
}
