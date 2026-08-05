package systems.zlink.framework.runtime.configuration;

import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.Set;
import systems.zlink.framework.configuration.ZLinkMetadataPolicyBuilder;
import systems.zlink.framework.errors.ZLinkConfigurationException;

public final class ZLinkMetadataPolicyRegistration implements ZLinkMetadataPolicyBuilder {
    private final Set<String> sessionToActorKeys = new LinkedHashSet<>();
    private final Set<String> actorToSessionKeys = new LinkedHashSet<>();

    @Override
    public ZLinkMetadataPolicyBuilder allowSessionToActor(String key) {
        addKey(sessionToActorKeys, key);
        return this;
    }

    @Override
    public ZLinkMetadataPolicyBuilder allowActorToSession(String key) {
        addKey(actorToSessionKeys, key);
        return this;
    }

    public Set<String> sessionToActorKeys() {
        return Collections.unmodifiableSet(sessionToActorKeys);
    }

    public Set<String> actorToSessionKeys() {
        return Collections.unmodifiableSet(actorToSessionKeys);
    }

    private static void addKey(Set<String> keys, String key) {
        if (key == null || key.isBlank()) {
            throw new ZLinkConfigurationException("metadata key is required");
        }
        keys.add(key);
    }
}
