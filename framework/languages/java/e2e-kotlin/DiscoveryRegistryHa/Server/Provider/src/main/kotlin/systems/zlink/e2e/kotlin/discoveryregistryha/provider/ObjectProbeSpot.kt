package systems.zlink.e2e.kotlin.discoveryregistryha.provider

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import kotlinx.coroutines.delay
import systems.zlink.e2e.kotlin.discoveryregistryha.Contracts
import systems.zlink.e2e.kotlin.discoveryregistryha.provider.Support.ProviderEvidenceStore
import systems.zlink.framework.kotlin.ZLinkSuspendingInstanceSpot
import systems.zlink.framework.kotlin.addHandler
import systems.zlink.framework.spots.ZLinkInstanceSpotContext
import systems.zlink.framework.spots.ZLinkSpotRequestHandler

// SF-C5A: the delayed fixture keeps the authority row in CREATING while the
// public location query is exercised; it is not a production runtime delay.
class ObjectProbeSpot(
    override val context: ZLinkInstanceSpotContext,
    private val evidence: ProviderEvidenceStore,
) : ZLinkSuspendingInstanceSpot() {
    override fun configure() {
        context.handlers().addHandler<ObjectRequestHandler>()
    }

    override suspend fun onInitializeSuspending() {
        if (context.spotId() == "sf-c5a-creating") {
            delay(3_000)
        }
    }

    fun evidence(): ProviderEvidenceStore = evidence
}

class ObjectRequestHandler :
    ZLinkSpotRequestHandler<ObjectProbeSpot, Contracts.ObjectReq, Contracts.ObjectRes> {
    override fun handle(
        spot: ObjectProbeSpot,
        request: Contracts.ObjectReq,
    ): CompletionStage<Contracts.ObjectRes> = CompletableFuture.completedFuture(
        Contracts.ObjectRes(
            spot.context.spotId(),
            spot.evidence().providerRid,
            spot.context.objectGeneration(),
        ),
    )
}
