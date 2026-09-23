package systems.zlink.samples.kotlin.deliverydispatch.client

import java.net.URI
import java.time.Duration
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.runBlocking
import systems.zlink.framework.kotlin.ZLinkKotlinStreamAssert
import systems.zlink.framework.kotlin.ZLinkKotlinStreamConnector
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.request
import systems.zlink.httpclient.ZLinkHttpClient
import systems.zlink.httpclient.kotlin.fetch
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleNames
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleTopology
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.BindCourierSessionReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.BindCourierSessionRes
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.CourierDecisionMsg
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.CreateDeliveryReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.CreateDeliveryRes
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.DeliveryStatus
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.DeliveryStatusNotify
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.OfferDeliveryNotify
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.ServerAssertionReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.ServerAssertionRes
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.SubscribeDeliveryReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.SubscribeDeliveryRes
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamDispatchMode
import systems.zlink.stream.connector.ZLinkStreamMessage

fun main(args: Array<String>) {
    SampleTopology.configure(args)
    runBlocking { DeliveryDispatchClientScenario().run() }
}

class DeliveryDispatchClientScenario {
    suspend fun run() {
        runStreamRuntime()
    }

    private suspend fun runStreamRuntime() {
        val customer = createClient(SampleTopology.CustomerStreamEndpoint)
        val courierA = createClient(SampleTopology.CourierStreamEndpoint)
        val courierB = createClient(SampleTopology.CourierStreamEndpoint)
        try {
            customer.connect().await()
            courierA.connect().await()
            courierB.connect().await()

            val courierABound =
                courierA.request<BindCourierSessionRes>(BindCourierSessionReq("courier-a")).await()
            ZLinkKotlinStreamAssert.ensure(
                courierABound.courierId == "courier-a",
                "courier-a binding id mismatch",
            )
            println("deliverydispatch-bind=courier-a")
            val courierBBound =
                courierB.request<BindCourierSessionRes>(BindCourierSessionReq("courier-b")).await()
            ZLinkKotlinStreamAssert.ensure(
                courierBBound.courierId == "courier-b",
                "courier-b binding id mismatch",
            )
            println("deliverydispatch-bind=courier-b")

            runSuccessfulDelivery(customer, courierA, courierB)
            runReassignedDelivery(customer, courierA, courierB)
            runCandidatesExhaustedDelivery(customer, courierA, courierB)
            assertServerEvidence()
            println(SampleNames.CompletedMarker)
        } finally {
            customer.close().await()
            courierA.close().await()
            courierB.close().await()
        }
    }

    private suspend fun runSuccessfulDelivery(
        customer: ZLinkKotlinStreamConnector,
        courier: ZLinkKotlinStreamConnector,
        otherCourier: ZLinkKotlinStreamConnector,
    ) = coroutineScope {
        val deliveryId = "delivery-success"
        val offer =
            async(start = CoroutineStart.UNDISPATCHED) {
                courier
                    .waitFor<OfferDeliveryNotify>()
                    .where { message -> message.payload().deliveryId == deliveryId }
                    .await()
            }
        val noOtherCourierOffer =
            async(start = CoroutineStart.UNDISPATCHED) {
                otherCourier
                    .expectNone<OfferDeliveryNotify>(OfferDeliveryNotify::class.java.simpleName)
                    .within(Duration.ofSeconds(1))
                    .await()
            }
        // --8<-- [start:doc-e2e-sequence]
        val statuses =
            async(start = CoroutineStart.UNDISPATCHED) {
                customer
                    .waitForSequence<DeliveryStatusNotify>(
                        DeliveryStatusNotify::class.java.simpleName
                    )
                    .expect { message ->
                        matchesStatus(message, deliveryId, DeliveryStatus.Assigned)
                    }
                    .expect { message ->
                        matchesStatus(message, deliveryId, DeliveryStatus.Accepted)
                    }
                    .expect { message ->
                        matchesStatus(message, deliveryId, DeliveryStatus.PickedUp)
                    }
                    .expect { message ->
                        matchesStatus(message, deliveryId, DeliveryStatus.Delivered)
                    }
                    .await()
            }
        // --8<-- [end:doc-e2e-sequence]

        val subscribed =
            customer.request<SubscribeDeliveryRes>(SubscribeDeliveryReq(deliveryId)).await()
        ZLinkKotlinStreamAssert.ensure(
            subscribed.deliveryId == deliveryId,
            "success subscription id mismatch",
        )
        println("deliverydispatch-subscribe=$deliveryId")

        val created =
            post(
                path = "/deliveries",
                body =
                    CreateDeliveryReq(
                        deliveryId = deliveryId,
                        customerId = "customer-1",
                        pickupAddress = "Kitchen 12",
                        dropoffAddress = "Customer Lobby",
                    ),
                responseType = CreateDeliveryRes::class.java,
            )
        ZLinkKotlinStreamAssert.ensure(
            created.deliveryId == deliveryId,
            "created success delivery id mismatch",
        )
        println("deliverydispatch-create=$deliveryId")

        val courierOffer = offer.await().payload()
        println("deliverydispatch-offer=$deliveryId:${courierOffer.courierId}")
        courier
            .send(CourierDecisionMsg(courierOffer.deliveryId, courierOffer.courierId, true, null))
            .await()

        val notifications = statuses.await().map { it.payload() }
        ZLinkKotlinStreamAssert.ensure(
            notifications.all { it.courierId == "courier-a" },
            "success delivery status courier mismatch",
        )
        noOtherCourierOffer.await()
    }

    private suspend fun runReassignedDelivery(
        customer: ZLinkKotlinStreamConnector,
        courierA: ZLinkKotlinStreamConnector,
        courierB: ZLinkKotlinStreamConnector,
    ) = coroutineScope {
        val deliveryId = "delivery-reassign"
        val firstOffer =
            async(start = CoroutineStart.UNDISPATCHED) {
                courierA
                    .waitFor<OfferDeliveryNotify>()
                    .where { message ->
                        message.payload().deliveryId == deliveryId &&
                            message.payload().courierId == "courier-a"
                    }
                    .await()
            }
        val secondOffer =
            async(start = CoroutineStart.UNDISPATCHED) {
                courierB
                    .waitFor<OfferDeliveryNotify>()
                    .where { message ->
                        message.payload().deliveryId == deliveryId &&
                            message.payload().courierId == "courier-b"
                    }
                    .await()
            }
        val statuses =
            async(start = CoroutineStart.UNDISPATCHED) {
                customer
                    .waitForSequence<DeliveryStatusNotify>(
                        DeliveryStatusNotify::class.java.simpleName
                    )
                    .expect { message ->
                        matchesStatus(message, deliveryId, DeliveryStatus.Assigned)
                    }
                    .expect { message ->
                        matchesStatus(message, deliveryId, DeliveryStatus.Reassigned)
                    }
                    .expect { message ->
                        matchesStatus(message, deliveryId, DeliveryStatus.Accepted)
                    }
                    .expect { message ->
                        matchesStatus(message, deliveryId, DeliveryStatus.PickedUp)
                    }
                    .expect { message ->
                        matchesStatus(message, deliveryId, DeliveryStatus.Delivered)
                    }
                    .await()
            }

        val subscribed =
            customer.request<SubscribeDeliveryRes>(SubscribeDeliveryReq(deliveryId)).await()
        ZLinkKotlinStreamAssert.ensure(
            subscribed.deliveryId == deliveryId,
            "reassignment subscription id mismatch",
        )
        println("deliverydispatch-subscribe=$deliveryId")

        val created =
            post(
                path = "/deliveries",
                body =
                    CreateDeliveryReq(
                        deliveryId = deliveryId,
                        customerId = "customer-1",
                        pickupAddress = "Kitchen 12",
                        dropoffAddress = "Customer Lobby",
                    ),
                responseType = CreateDeliveryRes::class.java,
            )
        ZLinkKotlinStreamAssert.ensure(
            created.deliveryId == deliveryId,
            "created reassignment delivery id mismatch",
        )
        println("deliverydispatch-create=$deliveryId")

        val staleOffer = firstOffer.await().payload()
        println("deliverydispatch-offer=$deliveryId:courier-a")
        val acceptedOffer = secondOffer.await().payload()
        println("deliverydispatch-offer=$deliveryId:${acceptedOffer.courierId}")
        courierB
            .send(CourierDecisionMsg(acceptedOffer.deliveryId, acceptedOffer.courierId, true, null))
            .await()

        val notifications = statuses.await().map { it.payload() }
        ZLinkKotlinStreamAssert.ensure(
            notifications.first().courierId == "courier-a",
            "initial courier mismatch",
        )
        ZLinkKotlinStreamAssert.ensure(
            notifications.drop(1).all { it.courierId == "courier-b" },
            "reassigned courier mismatch",
        )
        courierA
            .send(CourierDecisionMsg(staleOffer.deliveryId, staleOffer.courierId, true, null))
            .await()
        println(SampleNames.ReassignmentMarker)
    }

    private suspend fun runCandidatesExhaustedDelivery(
        customer: ZLinkKotlinStreamConnector,
        courierA: ZLinkKotlinStreamConnector,
        courierB: ZLinkKotlinStreamConnector,
    ) = coroutineScope {
        val deliveryId = "delivery-exhausted"
        val firstOffer =
            async(start = CoroutineStart.UNDISPATCHED) {
                courierA
                    .waitFor<OfferDeliveryNotify>()
                    .where { message ->
                        message.payload().deliveryId == deliveryId &&
                            message.payload().courierId == "courier-a"
                    }
                    .await()
            }
        val secondOffer =
            async(start = CoroutineStart.UNDISPATCHED) {
                courierB
                    .waitFor<OfferDeliveryNotify>()
                    .where { message ->
                        message.payload().deliveryId == deliveryId &&
                            message.payload().courierId == "courier-b"
                    }
                    .await()
            }
        val statuses =
            async(start = CoroutineStart.UNDISPATCHED) {
                customer
                    .waitForSequence<DeliveryStatusNotify>(
                        DeliveryStatusNotify::class.java.simpleName
                    )
                    .expect { message ->
                        matchesStatus(message, deliveryId, DeliveryStatus.Assigned)
                    }
                    .expect { message ->
                        matchesStatus(message, deliveryId, DeliveryStatus.Reassigned)
                    }
                    .expect { message -> matchesStatus(message, deliveryId, DeliveryStatus.Failed) }
                    .await()
            }

        val subscribed =
            customer.request<SubscribeDeliveryRes>(SubscribeDeliveryReq(deliveryId)).await()
        ZLinkKotlinStreamAssert.ensure(
            subscribed.deliveryId == deliveryId,
            "exhausted subscription id mismatch",
        )

        val created =
            post(
                path = "/deliveries",
                body =
                    CreateDeliveryReq(
                        deliveryId = deliveryId,
                        customerId = "customer-1",
                        pickupAddress = "Kitchen 12",
                        dropoffAddress = "Customer Lobby",
                    ),
                responseType = CreateDeliveryRes::class.java,
            )
        ZLinkKotlinStreamAssert.ensure(
            created.deliveryId == deliveryId,
            "created exhausted delivery id mismatch",
        )

        val first = firstOffer.await().payload()
        courierA
            .send(CourierDecisionMsg(first.deliveryId, first.courierId, false, "declined"))
            .await()
        val second = secondOffer.await().payload()
        courierB
            .send(CourierDecisionMsg(second.deliveryId, second.courierId, false, "declined"))
            .await()

        val notifications = statuses.await().map { it.payload() }
        ZLinkKotlinStreamAssert.ensure(
            notifications.count { it.status == DeliveryStatus.Failed } == 1,
            "exhausted delivery did not reach Failed exactly once",
        )
    }

    private fun matchesStatus(
        message: ZLinkStreamMessage<DeliveryStatusNotify>,
        deliveryId: String,
        status: DeliveryStatus,
    ): Boolean {
        println(
            "deliverydispatch-status=${message.payload().deliveryId}:${message.payload().status}"
        )
        return message.payload().deliveryId == deliveryId && message.payload().status == status
    }

    private suspend fun assertServerEvidence() {
        val response =
            post(
                path = "/self-check/assert",
                body = ServerAssertionReq("delivery-success", "delivery-reassign"),
                responseType = ServerAssertionRes::class.java,
            )
        ZLinkKotlinStreamAssert.ensure(response.passed, "server delivery evidence failed")
        println(SampleNames.ServerEvidenceMarker)
    }

    private suspend fun <TResponse> post(
        path: String,
        body: Any,
        responseType: Class<TResponse>,
    ): TResponse {
        return ZLinkHttpClient.create(SampleTopology.DispatchHttpEndpoint)
            .post(path)
            .body(body)
            .fetch(responseType)
            .await()
    }

    private fun createClient(endpoint: String): ZLinkKotlinStreamConnector =
        ZLinkStreamConnectorFactory.create(
                ZLinkStreamConnectorOptions(
                    URI.create(endpoint),
                    ZLinkStreamDispatchMode.IMMEDIATE,
                    SampleTimings.RequestTimeout,
                    SampleTimings.RequestTimeout,
                    2,
                    Duration.ofSeconds(5),
                    64 * 1024,
                    64 * 1024,
                    true,
                    Duration.ofSeconds(1),
                    Duration.ofSeconds(5),
                    true,
                    Duration.ofMillis(250),
                    Duration.ofSeconds(5),
                    2.0,
                    false,
                    null,
                    null,
                    null,
                    null,
                )
            )
            .kotlin()
}
