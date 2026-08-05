package systems.zlink.framework.configuration;

public interface ZLinkMetadataPolicyBuilder {
    ZLinkMetadataPolicyBuilder allowSessionToActor(String key);

    ZLinkMetadataPolicyBuilder allowActorToSession(String key);
}
