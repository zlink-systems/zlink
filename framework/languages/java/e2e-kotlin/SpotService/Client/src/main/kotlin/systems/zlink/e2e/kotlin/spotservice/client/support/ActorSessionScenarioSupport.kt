package systems.zlink.e2e.kotlin.spotservice.client.support

import systems.zlink.framework.kotlin.*

import java.time.Duration
import java.util.UUID
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CopyOnWriteArrayList
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.supervisorScope
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env

internal class ActorSessionScenarioContext {
    private val connector = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
    private val unbound = createStreamConnector(Env.get("e2e.stream.a.endpoint"))
    private val inboundNames = CopyOnWriteArrayList<String>()
    private val inboundObserver = connector.observeInbound { observation ->
        inboundNames.add(observation.packetName())
        CompletableFuture.completedFuture(null)
    }
    val actorId: String = "actor-local-" + UUID.randomUUID().toString().replace("-", "")

    val profile = Contracts.ActorProfile(
        "Player One",
        7,
        listOf("alpha", "beta"),
    )
    lateinit var auth: Contracts.ActorAuthRes
        private set
    lateinit var entryReply: Contracts.ActorEchoRes
        private set
    lateinit var entryPush: Contracts.ActorPushNotify
        private set
    lateinit var joined: Contracts.ActorJoinRes
        private set
    lateinit var userReply1: Contracts.ActorEchoRes
        private set
    lateinit var userPush1: Contracts.ActorPushNotify
        private set
    lateinit var userReply2: Contracts.ActorEchoRes
        private set
    lateinit var userPush2: Contracts.ActorPushNotify
        private set
    lateinit var userReply3: Contracts.ActorEchoRes
        private set
    lateinit var userPush3: Contracts.ActorPushNotify
        private set

    fun observedInboundNames(): List<String> = inboundNames.toList()

    suspend fun connectAndAuthenticate() {
        connector.connect().await()
        unbound.connect().await()
        auth = connector
            .request(Contracts.ActorAuthReq(actorId, profile))
            .awaitReply<Contracts.ActorAuthRes>()
    }

    suspend fun requestEntryEcho() = coroutineScope {
        val push = async(start = CoroutineStart.UNDISPATCHED) {
            connector.waitFor<Contracts.ActorPushNotify>().await()
        }
        entryReply = connector
            .request(Contracts.ActorEchoReq("entry-echo", 1, profile))
            .metadata("actor-id", actorId)
            .awaitReply<Contracts.ActorEchoRes>()
        entryPush = push.await().payload()
    }

    suspend fun joinRoom() {
        joined = connector
            .request(Contracts.ActorJoinReq("actor-room-a", profile, profile.tags))
            .metadata("actor-id", actorId)
            .awaitReply<Contracts.ActorJoinRes>()
    }

    suspend fun requestFirstUserEchoAndCheckUnboundIsolation() = supervisorScope {
        val unboundPush = async(start = CoroutineStart.UNDISPATCHED) {
            unbound.waitFor<Contracts.ActorPushNotify>()
                .timeout(Duration.ofMillis(400))
                .await()
        }
        val push = async(start = CoroutineStart.UNDISPATCHED) {
            connector.waitFor<Contracts.ActorPushNotify>().await()
        }
        userReply1 = requestUserEcho("user-echo-1", 2)
        userPush1 = push.await().payload()
        expectFailure { unboundPush.await() }
    }

    suspend fun requestOrderedUserEchoes() = coroutineScope {
        val secondPush = async(start = CoroutineStart.UNDISPATCHED) {
            connector.waitFor<Contracts.ActorPushNotify>().await()
        }
        userReply2 = requestUserEcho("user-echo-2", 3)
        userPush2 = secondPush.await().payload()

        val thirdPush = async(start = CoroutineStart.UNDISPATCHED) {
            connector.waitFor<Contracts.ActorPushNotify>().await()
        }
        userReply3 = requestUserEcho("user-echo-3", 4)
        userPush3 = thirdPush.await().payload()
    }

    private suspend fun requestUserEcho(value: String, seq: Int): Contracts.ActorEchoRes =
        connector
            .request(Contracts.ActorEchoReq(value, seq, profile))
            .metadata("actor-id", actorId)
            .awaitReply<Contracts.ActorEchoRes>()

    suspend fun close() {
        closeQuietly(inboundObserver)
        closeQuietly(connector)
        closeQuietly(unbound)
    }

    private fun closeQuietly(registration: AutoCloseable) {
        try {
            registration.close()
        } catch (_: Exception) {
        }
    }

    private suspend fun closeQuietly(stream: ZLinkKotlinStreamConnector) {
        try {
            stream.close().await()
        } catch (_: Exception) {
        }
    }
}

internal object ActorSessionScenarioSupport {
    suspend fun run(block: suspend (ActorSessionScenarioContext) -> Unit) {
        val context = ActorSessionScenarioContext()
        try {
            block(context)
            waitForSessionEvidence()
        } catch (error: Exception) {
            throw IllegalStateException("actor/session scenario failed", error)
        } finally {
            context.close()
        }
    }

    private fun waitForSessionEvidence() {
        postJson(
            Env.get("e2e.http.session.endpoint"),
            "/evidence/wait",
            Contracts.EvidenceWaitReq(
                listOf(
                    "ActorCreated",
                    "ActorCreatedPayload",
                    "Player One/7/alpha,beta",
                    "ActorUserJoined",
                    "ActorUserRequest",
                    "user-echo-3",
                    "StreamInbound",
                ),
                10_000,
            ),
            Contracts.EvidenceSnapshot::class.java,
        )
    }
}
