package systems.zlink.framework.configuration;

import java.util.function.Consumer;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkSpot;

public interface ZLinkMeshObjectServerBuilder {
    ZLinkMeshObjectServerBuilder addEntrySpot(
        Class<? extends ZLinkEntrySpot<?>> entrySpotType);

    <TSpot extends ZLinkSpot<?>> ZLinkMeshObjectServerBuilder addSpotFactory(
        String stableType,
        Class<TSpot> spotType,
        Consumer<ZLinkUserSpotFactoryBuilder<TSpot>> configure);

    <TSpot extends ZLinkInstanceSpot>
    ZLinkMeshObjectServerBuilder addInstanceSpotFactory(
        String stableType,
        Class<TSpot> spotType,
        Consumer<ZLinkInstanceSpotFactoryBuilder<TSpot>> configure);

    <TActor extends ZLinkActor> ZLinkMeshObjectServerBuilder addActorFactory(
        String stableType,
        Class<TActor> actorType,
        Class<? extends ZLinkActorFactory> factoryType,
        Consumer<ZLinkActorFactoryBuilder<TActor>> configure);
}
