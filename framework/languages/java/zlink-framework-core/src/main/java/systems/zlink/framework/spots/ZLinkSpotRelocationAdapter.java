package systems.zlink.framework.spots;

import systems.zlink.framework.actors.ZLinkRelocationCancellation;

import java.util.concurrent.CompletionStage;

/** Captures and restores application-owned Spot state during relocation. */
public interface ZLinkSpotRelocationAdapter<TSpot> {
    CompletionStage<byte[]> capture(TSpot spot, ZLinkRelocationCancellation cancellation);

    CompletionStage<Void> restore(
            TSpot spot, byte[] state, ZLinkRelocationCancellation cancellation);
}
