package systems.zlink.framework.actors;

import java.util.concurrent.CompletionStage;

/**
 * Captures and restores application-owned actor state during relocation.
 */
public interface ZLinkActorRelocationAdapter<TActor extends ZLinkActor> {
    CompletionStage<byte[]> capture(
        TActor actor,
        ZLinkRelocationCancellation cancellation);

    CompletionStage<Void> restore(
        TActor actor,
        byte[] state,
        ZLinkRelocationCancellation cancellation);
}
