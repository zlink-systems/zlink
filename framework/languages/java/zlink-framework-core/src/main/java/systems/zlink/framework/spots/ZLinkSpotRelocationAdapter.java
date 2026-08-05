package systems.zlink.framework.spots;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;

/**
 * Captures and restores application-owned Spot state during relocation.
 */
public interface ZLinkSpotRelocationAdapter<TSpot> {
    CompletionStage<byte[]> capture(
        TSpot spot,
        ZLinkRelocationCancellation cancellation);

    CompletionStage<Void> restore(
        TSpot spot,
        byte[] state,
        ZLinkRelocationCancellation cancellation);
}
