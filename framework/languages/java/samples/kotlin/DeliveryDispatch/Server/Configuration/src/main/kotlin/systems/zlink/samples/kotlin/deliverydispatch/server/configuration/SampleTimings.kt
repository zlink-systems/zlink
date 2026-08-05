package systems.zlink.samples.kotlin.deliverydispatch.server.configuration

import java.time.Duration

object SampleTimings {
    val RequestTimeout: Duration = Duration.ofSeconds(10)
    /**
     * How long an offer stands before the sweeper reassigns it. The deadline belongs to the
     * dispatch worker, not to the courier node (common sample spec section 7.4).
     */
    val CourierDecisionTimeout: Duration = Duration.ofMillis(1500)
    val OfferSweepInterval: Duration = Duration.ofMillis(100)
}
