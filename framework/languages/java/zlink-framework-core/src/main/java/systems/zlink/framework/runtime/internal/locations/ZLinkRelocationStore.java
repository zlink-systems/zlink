package systems.zlink.framework.runtime.internal.locations;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;

/**
 * Framework-private relocation repository used after provider adaptation.
 */
public interface ZLinkRelocationStore {
    CompletionStage<ZLinkRelocationStored> put(
        byte[] payload,
        Duration retention,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkRelocationReadResult> get(
        String reference,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkRelocationRenewResult> renew(
        String reference,
        Duration retention,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkRelocationDeleteResult> delete(
        String reference,
        ZLinkStoreCancellation cancellation);
}
