package systems.zlink.framework;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.Arrays;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.configuration.ZLinkActorFactoryBuilder;
import systems.zlink.framework.configuration.ZLinkInstanceSpotFactoryBuilder;
import systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder;
import systems.zlink.framework.configuration.ZLinkFrameworkOptions;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.configuration.ZLinkMeshObjectRoleBuilder;
import systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder;
import systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkSpotCloseReason;
import systems.zlink.framework.spots.ZLinkSpotClosingContext;
import systems.zlink.framework.spots.ZLinkSpotRelocationAdapter;
import systems.zlink.framework.spots.ZLinkSpotRelocationReadyCall;
import systems.zlink.framework.spots.ZLinkSpotRelocationReadyCompletion;
import systems.zlink.framework.spots.ZLinkSpotRelocationReadyOutcome;

final class RelocationPublicContractTest {
    @Test
    void exposesSeparatedRelocationStoreRegistration() throws Exception {
        assertEquals(
            void.class,
            ZLinkFrameworkOptions.class
                .getMethod(
                    "addRelocationStore",
                    systems.zlink.framework.locationprovider
                        .ZLinkRelocationStore.class)
                .getReturnType());
        assertEquals(
            CompletionStage.class,
            systems.zlink.framework.locationprovider.ZLinkRelocationStore.class
                .getMethod(
                    "put",
                    systems.zlink.framework.locationprovider
                        .ZLinkBlobReference.class,
                    byte[].class,
                    Duration.class,
                    systems.zlink.framework.locationprovider
                        .ZLinkStoreCancellation.class)
                .getReturnType());
        assertEquals(
            CompletionStage.class,
            systems.zlink.framework.locationprovider.ZLinkRelocationStore.class
                .getMethod(
                    "read",
                    systems.zlink.framework.locationprovider
                        .ZLinkBlobReference.class,
                    systems.zlink.framework.locationprovider
                        .ZLinkStoreCancellation.class)
                .getReturnType());
        assertEquals(
            CompletionStage.class,
            systems.zlink.framework.locationprovider.ZLinkRelocationStore.class
                .getMethod(
                    "renew",
                    systems.zlink.framework.locationprovider
                        .ZLinkBlobReference.class,
                    Duration.class,
                    systems.zlink.framework.locationprovider
                        .ZLinkStoreCancellation.class)
                .getReturnType());
        assertEquals(
            CompletionStage.class,
            systems.zlink.framework.locationprovider.ZLinkRelocationStore.class
                .getMethod(
                    "delete",
                    systems.zlink.framework.locationprovider
                        .ZLinkBlobReference.class,
                    systems.zlink.framework.locationprovider
                        .ZLinkStoreCancellation.class)
                .getReturnType());
        for (String removedType : List.of(
            "systems.zlink.framework.locations.ZLinkLocationStore",
            "systems.zlink.framework.locations.ZLinkRelocationStore",
            "systems.zlink.framework.locations.ZLinkRelocationStored",
            "systems.zlink.framework.locations.ZLinkRelocationReadResult",
            "systems.zlink.framework.locations.ZLinkRelocationRenewResult",
            "systems.zlink.framework.locations.ZLinkRelocationDeleteResult")) {
            assertThrows(
                ClassNotFoundException.class,
                () -> Class.forName(removedType));
        }
    }

    @Test
    void exposesRelocationAdaptersAndInstanceSpotLifecycle()
        throws Exception {
        assertEquals(
            CompletionStage.class,
            ZLinkActorRelocationAdapter.class
                .getMethod(
                    "capture",
                    systems.zlink.framework.actors.ZLinkActor.class,
                    ZLinkRelocationCancellation.class)
                .getReturnType());
        assertEquals(
            CompletionStage.class,
            ZLinkSpotRelocationAdapter.class
                .getMethod(
                    "restore",
                    Object.class,
                    byte[].class,
                    ZLinkRelocationCancellation.class)
                .getReturnType());
        assertEquals(
            CompletionStage.class,
            ZLinkInstanceSpot.class
                .getMethod("onClosing", ZLinkSpotClosingContext.class)
                .getReturnType());
        assertEquals(2, ZLinkSpotCloseReason.RELOCATION_OUT.value());
        assertEquals(3, ZLinkSpotCloseReason.IDLE_EVICTED.value());
        assertEquals(
            void.class,
            ZLinkActorFactoryBuilder.class
                .getMethod("preserveStateWith", Class.class)
                .getReturnType());
        assertEquals(
            void.class,
            ZLinkUserSpotFactoryBuilder.class
                .getMethod("recreateOnRelocation")
                .getReturnType());
        assertEquals(
            void.class,
            ZLinkInstanceSpotFactoryBuilder.class
                .getMethod("disableRelocation")
                .getReturnType());
        assertEquals(
            ZLinkMeshObjectServerBuilder.class,
            ZLinkMeshObjectServerBuilder.class
                .getMethod(
                    "addActorFactory",
                    String.class,
                    Class.class,
                    Class.class,
                    Consumer.class)
                .getReturnType());
        assertEquals(0, ZLinkSpotRelocationReadinessMode.ANY_TURN_BOUNDARY.value());
        assertEquals(1, ZLinkSpotRelocationReadinessMode.APPLICATION_SIGNALED.value());
        assertEquals(0, ZLinkSpotRelocationReadyOutcome.CONTINUED.value());
        assertEquals(1, ZLinkSpotRelocationReadyOutcome.RELOCATED.value());
        assertEquals(
            ZLinkSpotRelocationReadyOutcome.RELOCATED,
            new ZLinkSpotRelocationReadyCompletion(
                ZLinkSpotRelocationReadyOutcome.RELOCATED).outcome());
        assertEquals(
            void.class,
            ZLinkSpotRelocationReadyCall.class
                .getMethod("defer").getReturnType());
        assertThrows(
            ClassNotFoundException.class,
            () -> Class.forName(
                "systems.zlink.framework.actors.ZLinkRelocationPolicy"));
        assertThrows(
            ClassNotFoundException.class,
            () -> Class.forName(
                "systems.zlink.framework.configuration.ZLinkActorFactoryOptions"));
        assertThrows(
            ClassNotFoundException.class,
            () -> Class.forName(
                "systems.zlink.framework.configuration.ZLinkUserSpotFactoryOptions"));
        assertThrows(
            ClassNotFoundException.class,
            () -> Class.forName(
                "systems.zlink.framework.configuration.ZLinkInstanceSpotFactoryOptions"));
        assertEquals(
            ZLinkMeshObjectRoleBuilder.class,
            ZLinkMeshNodeBuilder.class.getMethod("objects").getReturnType());
        assertEquals(
            ZLinkMeshObjectServerBuilder.class,
            ZLinkMeshObjectRoleBuilder.class.getMethod("server").getReturnType());
        assertEquals(
            void.class,
            ZLinkFrameworkOptions.class.getMethod(
                "addLocationStore",
                systems.zlink.framework.locationprovider
                    .ZLinkLocationStore.class).getReturnType());
    }
}
