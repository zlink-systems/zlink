package systems.zlink.framework.perf;

import systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder;

public final class PerfActorType {
    private PerfActorType() {}

    public static final String NAME = "perf-actor";

    /** Actors are created as Entry Spot members and are never moved (perf never measures relocation). */
    public static ZLinkMeshObjectServerBuilder addPerfActors(ZLinkMeshObjectServerBuilder objects) {
        return objects
                .addEntrySpot(PerfEntrySpot.class)
                .addActorFactory(NAME, PerfActor.class, PerfActorFactory.class, factory -> factory.disableRelocation());
    }
}
