package systems.zlink.framework.actors;

public interface ZLinkActorClient {
    ZLinkActorSendCall sendToActor(String actorId, Object message);

    ZLinkActorRequestCall requestToActor(String actorId, Object request);
}
