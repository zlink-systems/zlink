package systems.zlink.framework.kotlin

import java.time.Duration
import java.util.Optional
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertSame
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test
import org.junit.jupiter.api.assertThrows
import systems.zlink.contracts.core.RoutingId
import systems.zlink.contracts.messaging.Message
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorDirectory
import systems.zlink.framework.actors.ZLinkActorCreateCall
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorGetOrCreateCall
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.actors.ZLinkActorRequestCall
import systems.zlink.framework.actors.ZLinkActorSendCall
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.channels.ZLinkRequestCall
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.channels.ZLinkSendCall
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind
import systems.zlink.framework.errors.ZLinkFrameworkException
import systems.zlink.framework.errors.ZLinkConfigurationException
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore
import systems.zlink.framework.spots.SpotRef
import systems.zlink.framework.spots.ZLinkSpotCreateCall
import systems.zlink.framework.spots.ZLinkSpotCreateResult
import systems.zlink.framework.spots.ZLinkSpotCreateState
import systems.zlink.framework.spots.ZLinkSpotGetOrCreateCall
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.framework.streams.ZLinkSessionActor

class KotlinFrameworkExtensionsContractTest {
    @Test
    fun `Kotlin manager wrappers preserve fluent state and coroutine terminals`() = runBlocking {
        val actorManager = RecordingActorManager()
        val spotManager = RecordingStableSpotManager()

        val actor = actorManager.kotlin()
            .create("actor-a", "player")
            .inMesh("mesh-a")
            .request(CreateActor("actor"))
            .timeout(Duration.ofSeconds(2))
            .await()
        val spot = spotManager.kotlin()
            .getOrCreate(SPOT_ID, "room")
            .inMesh("mesh-a")
            .request(CreateActor("spot"))
            .timeout(Duration.ofSeconds(3))
            .yield()

        assertEquals(ACTOR_REF, (actor as ZLinkActorCreateResult.Created).actor())
        assertEquals(SPOT_REF, spot.spot())
        assertEquals(listOf("mesh-a", "PT2S"), actorManager.options)
        assertTrue(spotManager.lastGetOrCreateCall.yielded)
    }

    @Test
    fun `stable type spot call builders preserve options and are single use`() {
        val manager = RecordingStableSpotManager()

        val created = manager.create("room-v1")
            .inMesh("mesh-a")
            .request(ZLinkMessage.of(CreateActor("create")))
            .timeout(Duration.ofSeconds(3))
            .submit()
            .toCompletableFuture()
            .join()
        val existing = manager.getOrCreate(SPOT_ID, "room-v1")
            .request(CreateActor("get-or-create"))
            .submit()
            .toCompletableFuture()
            .join()

        assertEquals(SPOT_REF, created.spot())
        assertEquals(SPOT_REF, existing.spot())
        assertEquals(
            listOf("mesh-a", "PT3S"),
            manager.createOptions,
        )
        assertThrows<IllegalStateException> { manager.lastCreateCall.submit() }
        assertThrows<IllegalStateException> { manager.lastGetOrCreateCall.submit() }
    }

    @Test
    fun `fanout automatic and manual subscriber configuration is rejected`() {
        val options = DefaultZLinkFrameworkOptions()
        options.addLocationStore(ZLinkInMemoryLocationStore())
        val channel = options.addFanoutChannel("events")
        channel.enableSubscriber()
        channel.subscriberConnections().connect("inproc://events")

        assertThrows<ZLinkConfigurationException> { options.validate() }
    }

    @Test
    fun `coroutine cancellation projects to completion stage cancel false`() = runBlocking {
        val stage = RecordingCancellationFuture<String>()
        val job = launch(start = CoroutineStart.UNDISPATCHED) {
            awaitFrameworkStage(stage)
        }

        job.cancelAndJoin()

        assertTrue(stage.isCancelled)
        assertFalse(stage.mayInterruptIfRunning)
        assertEquals(1, stage.cancellations)
    }

    @Test
    fun `coroutine cancellation does not discard a stage that already rejected cancellation`() = runBlocking {
        val stage = CommittedResultFuture<String>()
        val job = launch(start = CoroutineStart.UNDISPATCHED) {
            awaitFrameworkStage(stage)
        }

        job.cancelAndJoin()
        stage.complete("committed")

        assertTrue(job.isCancelled)
        assertEquals(1, stage.cancellations)
        assertFalse(stage.isCancelled)
        assertEquals("committed", stage.join())
    }

    @Test
    fun `directory object ensure extension delegates to Java ensure overload`() = runBlocking {
        val directory = RecordingActorDirectory(ACTOR_REF)

        val result = directory.ensureActor("actor-a", CreateActor("payload"))

        assertEquals(ACTOR_REF, result)
        assertEquals("actor-a", directory.actorId)
        assertEquals(CreateActor("payload"), directory.request?.decode(CreateActor::class.java))
    }

    @Test
    fun `actor ref snapshot extension delegates to shared snapshot model`() {
        val snapshot = ACTOR_REF.snapshot()

        assertEquals(ACTOR_REF.nodeRid(), snapshot.nodeRid())
        assertEquals(ACTOR_REF.actorId(), snapshot.actorId())
        assertEquals(ACTOR_REF.objectGeneration(), snapshot.objectGeneration())
        assertEquals(ACTOR_REF.meshName(), snapshot.meshName())
        assertEquals(ACTOR_REF, snapshot.actorRef())
    }

    @Test
    fun `actor request extension delegates to Java global actor id call`() = runBlocking {
        val actorClient = RecordingActorClient(ActorReply("reply"))

        val reply = actorClient.requestToActorAwait<ActorReply>(
            ACTOR_REF.actorId(),
            ActorMessage("request"),
        )

        assertEquals(ActorReply("reply"), reply)
        assertEquals(ACTOR_REF.actorId(), actorClient.requestedActorId)
        assertEquals(ActorMessage("request"), actorClient.requestedMessage)
    }

    @Test
    fun `framework error kind is preserved across coroutine await boundary`() = runBlocking {
        val call = FailingRequestCall(
            ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                "stale actor location",
            ),
        )

        val error = assertThrows<ZLinkFrameworkException> {
            runBlocking {
                call.awaitReply<String>()
            }
        }

        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, error.kind())
    }

    @Test
    fun `session actor logical disconnect awaits only the selected exact binding`() = runBlocking {
        val oldBinding = RecordingSessionActor(
            ActorRef("actor-a", 7, "mesh-a", NODE_RID),
        )
        val newIncarnation = RecordingSessionActor(
            ActorRef("actor-a", 8, "mesh-a", NODE_RID),
        )

        awaitFrameworkStage(oldBinding.notifyDisconnected())
        awaitFrameworkStage(oldBinding.notifyDisconnected())

        assertEquals(1, oldBinding.disconnects)
        assertEquals(0, newIncarnation.disconnects)
        assertEquals(7, oldBinding.ref().objectGeneration())
        assertEquals(8, newIncarnation.ref().objectGeneration())
    }

    private data class CreateActor(val value: String)

    private data class ActorMessage(val value: String)

    private data class ActorReply(val value: String)

    private class RecordingCancellationFuture<T> : CompletableFuture<T>() {
        var cancellations = 0
        var mayInterruptIfRunning = true

        override fun cancel(mayInterruptIfRunning: Boolean): Boolean {
            cancellations++
            this.mayInterruptIfRunning = mayInterruptIfRunning
            return super.cancel(mayInterruptIfRunning)
        }
    }

    private class CommittedResultFuture<T> : CompletableFuture<T>() {
        var cancellations = 0

        override fun cancel(mayInterruptIfRunning: Boolean): Boolean {
            cancellations++
            return false
        }
    }

    private class RecordingActorDirectory(
        private val actorRef: ActorRef,
    ) : ZLinkActorDirectory {
        var actorId: String? = null
        var request: ZLinkMessage? = null

        override fun find(actorId: String): CompletionStage<Optional<ActorRef>> =
            CompletableFuture.completedFuture(Optional.empty())

        override fun ensure(
            actorId: String,
            createRequest: ZLinkMessage,
        ): CompletionStage<ActorRef> {
            this.actorId = actorId
            this.request = createRequest
            return CompletableFuture.completedFuture(actorRef)
        }
    }

    private class RecordingSendCall : ZLinkSendCall {
        override fun submit(): CompletionStage<Void> =
            CompletableFuture.completedFuture(null)
    }

    private class RecordingRequestCall<TReply>(
        private val reply: TReply,
    ) : ZLinkRequestCall {
        override fun timeout(timeout: Duration): ZLinkRequestCall = this

        override fun <T : Any?> submit(replyType: Class<T>): CompletionStage<T> =
            CompletableFuture.completedFuture(replyType.cast(reply))

        override fun <T : Any?> yield(replyType: Class<T>): CompletionStage<T> =
            submit(replyType)
    }

    private class RecordingActorClient<TReply>(
        private val reply: TReply,
    ) : ZLinkActorClient {
        var sentActorId: String? = null
        var sentMessage: Any? = null
        var requestedActorId: String? = null
        var requestedMessage: Any? = null

        override fun sendToActor(actorId: String, message: Any): ZLinkActorSendCall {
            sentActorId = actorId
            sentMessage = message
            return RecordingActorSendCall()
        }

        override fun requestToActor(actorId: String, request: Any): ZLinkActorRequestCall {
            requestedActorId = actorId
            requestedMessage = request
            return RecordingActorRequestCall(reply)
        }
    }

    private class RecordingActorSendCall : ZLinkActorSendCall {
        override fun metadata(key: String, value: String): ZLinkActorSendCall = this

        override fun submit(): CompletionStage<Void> =
            CompletableFuture.completedFuture(null)
    }

    private class RecordingActorRequestCall<TReply>(
        private val reply: TReply,
    ) : ZLinkActorRequestCall {
        override fun metadata(key: String, value: String): ZLinkActorRequestCall = this

        override fun timeout(timeout: Duration): ZLinkActorRequestCall = this

        override fun <T : Any?> submit(replyType: Class<T>): CompletionStage<T> =
            CompletableFuture.completedFuture(replyType.cast(reply))

        override fun <T : Any?> yield(replyType: Class<T>): CompletionStage<T> =
            submit(replyType)
    }

    private class FailingRequestCall(
        private val error: Throwable,
    ) : ZLinkRequestCall {
        override fun timeout(timeout: Duration): ZLinkRequestCall = this

        override fun <T : Any?> submit(replyType: Class<T>): CompletionStage<T> {
            val future = CompletableFuture<T>()
            future.completeExceptionally(error)
            return future
        }

        override fun <T : Any?> yield(replyType: Class<T>): CompletionStage<T> =
            submit(replyType)
    }

    private class TestActor : ZLinkActor {
        override fun context(): ZLinkActorContext =
            throw UnsupportedOperationException("test actor has no runtime context")
    }

    private class RecordingSessionActor(
        private val actorRef: ActorRef,
    ) : ZLinkSessionActor {
        private var disconnect: CompletableFuture<Void>? = null
        var disconnects = 0

        override fun actorId(): String = actorRef.actorId()

        override fun ref(): ActorRef = actorRef

        override fun relay(payload: ZLinkMessage): CompletionStage<Void> =
            CompletableFuture.completedFuture(null)

        override fun notifyDisconnected(): CompletionStage<Void> {
            disconnect?.let { return it }
            disconnects++
            return CompletableFuture.completedFuture<Void>(null).also {
                disconnect = it
            }
        }
    }

    private class RecordingStableSpotManager : ZLinkSpotManager {
        val createOptions = mutableListOf<String>()
        lateinit var lastCreateCall: RecordingCreateCall
        lateinit var lastGetOrCreateCall: RecordingGetOrCreateCall

        override fun create(spotType: String): ZLinkSpotCreateCall =
            RecordingCreateCall(createOptions).also { lastCreateCall = it }

        override fun getOrCreate(
            spotId: String,
            spotType: String,
        ): ZLinkSpotGetOrCreateCall =
            RecordingGetOrCreateCall().also { lastGetOrCreateCall = it }

        override fun find(spotId: String): CompletionStage<Optional<SpotRef>> =
            CompletableFuture.completedFuture(Optional.of(SPOT_REF))

        override fun close(spot: SpotRef): CompletionStage<Boolean> =
            CompletableFuture.completedFuture(true)
    }

    private class RecordingCreateCall(
        private val options: MutableList<String>,
    ) : ZLinkSpotCreateCall {
        private var submitted = false

        override fun inMesh(meshName: String) = apply { options += meshName }
        override fun request(request: Any) = apply {}
        override fun request(request: ZLinkMessage) = apply {}
        override fun timeout(timeout: Duration) =
            apply { options += timeout.toString() }
        override fun submit(): CompletionStage<ZLinkSpotCreateResult> {
            check(!submitted)
            submitted = true
            return CompletableFuture.completedFuture(RESULT)
        }
        override fun yield(): CompletionStage<ZLinkSpotCreateResult> =
            submit()
    }

    private class RecordingGetOrCreateCall : ZLinkSpotGetOrCreateCall {
        private var submitted = false
        var yielded = false

        override fun inMesh(meshName: String) = this
        override fun request(request: Any) = this
        override fun request(request: ZLinkMessage) = this
        override fun timeout(timeout: Duration) = this
        override fun submit(): CompletionStage<ZLinkSpotCreateResult> {
            check(!submitted)
            submitted = true
            return CompletableFuture.completedFuture(RESULT)
        }
        override fun yield(): CompletionStage<ZLinkSpotCreateResult> {
            yielded = true
            return submit()
        }
    }

    private class RecordingActorManager : ZLinkActorManager {
        val options = mutableListOf<String>()

        override fun create(
            actorId: String,
            actorType: String,
        ): ZLinkActorCreateCall = RecordingActorCall(options)

        override fun getOrCreate(
            actorId: String,
            actorType: String,
        ): ZLinkActorGetOrCreateCall = RecordingActorCall(options)

        override fun find(actorId: String): CompletionStage<Optional<ActorRef>> =
            CompletableFuture.completedFuture(Optional.of(ACTOR_REF))

        override fun findSpot(
            actorId: String,
        ): CompletionStage<Optional<systems.zlink.framework.spots.SpotRef>> =
            CompletableFuture.completedFuture(Optional.empty())
    }

    private class RecordingActorCall(
        private val options: MutableList<String>,
    ) : ZLinkActorCreateCall, ZLinkActorGetOrCreateCall {
        private var submitted = false

        override fun inMesh(meshName: String) =
            apply { options += meshName }

        override fun request(request: Any) = this

        override fun request(request: ZLinkMessage) = this

        override fun timeout(timeout: Duration) =
            apply { options += timeout.toString() }

        override fun submit(): CompletionStage<ZLinkActorCreateResult> {
            check(!submitted)
            submitted = true
            return CompletableFuture.completedFuture(
                ZLinkActorCreateResult.Created(ACTOR_REF, ZLinkMessage.empty()),
            )
        }

        override fun yield(): CompletionStage<ZLinkActorCreateResult> =
            submit()
    }

    companion object {
        private val NODE_RID: RoutingId = RoutingId.from(byteArrayOf(0x01))
        private const val SPOT_ID: String = "spot-02"
        private val SPOT_REF = SpotRef(SPOT_ID, 7, "mesh-a", NODE_RID)
        private val RESULT = ZLinkSpotCreateResult(
            SPOT_REF,
            ZLinkSpotCreateState.CREATED,
            ZLinkMessage.empty(),
        )
        private val ACTOR_REF = ActorRef("actor-a", 7, "mesh-a", NODE_RID)
    }
}
