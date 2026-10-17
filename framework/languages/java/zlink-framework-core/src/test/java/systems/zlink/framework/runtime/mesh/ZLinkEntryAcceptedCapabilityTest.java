package systems.zlink.framework.runtime.mesh;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkMeshNodeMonitoringProjection;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/** The published {@code entryAccepted} capability and the target refusal share one decision. */
final class ZLinkEntryAcceptedCapabilityTest {
    @Test
    void nodeWithoutEntrySpotAcceptsNoNewActor() {
        MeshNodeRegistration mesh = registration(false, false);

        assertFalse(mesh.acceptsNewActor("player"));
        assertFalse(mesh.acceptsNewActor("monster"));
        assertEquals(false, entryAccepted(mesh, "player"));
        assertEquals(false, entryAccepted(mesh, "monster"));
    }

    @Test
    void entrySpotForAnotherActorTypeDoesNotAcceptThisType() {
        MeshNodeRegistration mesh = registration(false, true);

        assertFalse(mesh.acceptsNewActor("player"));
        assertTrue(mesh.acceptsNewActor("monster"));
        assertEquals(false, entryAccepted(mesh, "player"));
        assertEquals(true, entryAccepted(mesh, "monster"));
    }

    @Test
    void entrySpotForTheActorTypeAcceptsOnlyThatStableType() {
        MeshNodeRegistration mesh = registration(true, false);

        assertTrue(mesh.acceptsNewActor("player"));
        assertFalse(mesh.acceptsNewActor("monster"));
        assertEquals(true, entryAccepted(mesh, "player"));
        assertEquals(false, entryAccepted(mesh, "monster"));
    }

    @Test
    @SuppressWarnings({"unchecked", "rawtypes"})
    void entrySpotWhoseActorTypeCannotBeResolvedIsRejectedAtRegistration() {
        MeshNodeRegistration mesh = new MeshNodeRegistration("game");
        Class unresolved = GenericEntry.class;

        assertThrows(
                ZLinkConfigurationException.class,
                () -> mesh.objects().server().addEntrySpot(unresolved));
    }

    private static MeshNodeRegistration registration(boolean playerEntry, boolean monsterEntry) {
        MeshNodeRegistration mesh = new MeshNodeRegistration("game");
        var server = mesh.objects().server();
        server.addActorFactory("player", Player.class, Factory.class, f -> f.disableRelocation());
        server.addActorFactory("monster", Monster.class, Factory.class, f -> f.disableRelocation());
        if (playerEntry) {
            server.addEntrySpot(PlayerEntry.class);
        }
        if (monsterEntry) {
            server.addEntrySpot(MonsterEntry.class);
        }
        return mesh;
    }

    private static boolean entryAccepted(MeshNodeRegistration mesh, String stableType) {
        return ZLinkMeshNodeMonitoringProjection.capabilities(mesh).stream()
                .filter(
                        capability ->
                                capability.objectKind() == ZLinkPlacementObjectKind.ACTOR
                                        && capability.stableType().equals(stableType))
                .map(ZLinkObjectCapability::entryAccepted)
                .findFirst()
                .orElseThrow();
    }

    public static final class Player implements ZLinkActor {
        @Override
        public ZLinkActorContext context() {
            return null;
        }
    }

    public static final class Monster implements ZLinkActor {
        @Override
        public ZLinkActorContext context() {
            return null;
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player());
        }
    }

    public abstract static class BaseEntry<TActor extends ZLinkActor>
            implements ZLinkEntrySpot<TActor> {
        @Override
        public ZLinkEntrySpotContext context() {
            return null;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(TActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(TActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    /** The Actor type reaches the interface through a generic superclass. */
    public static final class PlayerEntry extends BaseEntry<Player> {}

    public static final class MonsterEntry extends BaseEntry<Monster> {}

    public static final class GenericEntry<TActor extends ZLinkActor> extends BaseEntry<TActor> {}
}
