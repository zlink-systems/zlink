package systems.zlink.framework.runtime.internal.configuration;

import java.util.Objects;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkSpot;

/**
 * Internal immutable factory registration values.
 */
public final class ZLinkObjectFactoryRegistration {
    private ZLinkObjectFactoryRegistration() {
    }

    public sealed interface RelocationPolicy
        permits RelocationPolicy.Disabled,
                RelocationPolicy.Recreate,
                RelocationPolicy.PreserveState {
        record Disabled() implements RelocationPolicy {
        }

        record Recreate() implements RelocationPolicy {
        }

        record PreserveState(Class<?> adapterClass)
            implements RelocationPolicy {
            public PreserveState {
                Objects.requireNonNull(adapterClass, "adapterClass");
            }
        }
    }

    public record UserSpotFactoryConfiguration(
        int stableTypeLimit,
        ZLinkUserSpotExecutionMode executionMode,
        ZLinkSpotRelocationReadinessMode relocationReadiness) {
    }

    public record InstanceSpotFactoryConfiguration(int stableTypeLimit) {
    }

    public record RelocatableSpotFactory<TSpot extends ZLinkSpot<?>>(
        String stableType,
        Class<TSpot> spotType,
        UserSpotFactoryConfiguration options,
        RelocationPolicy relocationPolicy) {
    }

    public record RelocatableInstanceSpotFactory<TSpot extends ZLinkInstanceSpot>(
        String stableType,
        Class<TSpot> spotType,
        InstanceSpotFactoryConfiguration options,
        RelocationPolicy relocationPolicy) {
    }

    public record RelocatableActorFactory<TActor extends ZLinkActor>(
        String stableType,
        Class<TActor> actorType,
        Class<? extends ZLinkActorFactory> factoryType,
        RelocationPolicy relocationPolicy) {
    }
}
