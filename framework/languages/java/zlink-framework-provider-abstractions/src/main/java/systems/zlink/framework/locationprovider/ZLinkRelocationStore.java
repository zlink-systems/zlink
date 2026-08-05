package systems.zlink.framework.locationprovider;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

public interface ZLinkRelocationStore {
    CompletionStage<ZLinkBlobPutResult> put(
        ZLinkBlobReference reference,
        byte[] payload,
        Duration retention,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkBlobReadResult> read(
        ZLinkBlobReference reference,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkBlobRenewResult> renew(
        ZLinkBlobReference reference,
        Duration retention,
        ZLinkStoreCancellation cancellation);

    CompletionStage<Void> delete(
        ZLinkBlobReference reference,
        ZLinkStoreCancellation cancellation);
}
