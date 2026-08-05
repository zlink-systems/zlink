package systems.zlink.framework.configuration;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;

/**
 * Configures an Actor factory's relocation behavior.
 *
 * @param <TActor> the Actor type created by the factory
 */
public interface ZLinkActorFactoryBuilder<TActor extends ZLinkActor> {
    void disableRelocation();

    void recreateOnRelocation();

    void preserveStateWith(
        Class<? extends ZLinkActorRelocationAdapter<TActor>> adapterClass);
}
