package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot

import java.lang.reflect.Proxy
import java.util.concurrent.CompletableFuture
import kotlinx.coroutines.runBlocking
import org.junit.jupiter.api.Assertions.*
import org.junit.jupiter.api.Test
import systems.zlink.framework.actors.*
import systems.zlink.framework.channels.*
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind
import systems.zlink.framework.errors.ZLinkFrameworkException
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*
import systems.zlink.samples.kotlin.bingo.server.play.domain.bingo.BingoRoomSettings
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors.PlayerActor
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers.BingoRoomSettingsInitializer
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.entryspot.BingoEntrySpot
import systems.zlink.samples.kotlin.bingo.shared.contracts.*

class BingoObserverJoinTest {
    private inline fun <reified T> publicStub(
        crossinline call: (String, Array<out Any?>) -> Any?
    ): T =
        Proxy.newProxyInstance(T::class.java.classLoader, arrayOf(T::class.java)) { _, method, args
            ->
            call(method.name, args ?: emptyArray())
        } as T

    @Test fun finishedRoomSnapshotDoesNotCompensateAnObserver() = observe()

    @Test
    fun closedRoomEndsObserverJoinButOtherRequestFailuresPropagate() {
        observe(ZLinkFrameworkException(ZLinkFrameworkErrorKind.NOT_FOUND, "Room closed"))
        observe(ZLinkFrameworkException(ZLinkFrameworkErrorKind.UNAVAILABLE, "Room unavailable"))
    }

    @Test
    fun observerRequestAfterRoomFinishDoesNotCreateAnObservationRoom() = runBlocking {
        val state =
            Messages.BingoRoomState.newBuilder().setRoomId("room").setStatus("Finished").build()
        val observation = Messages.GetBingoObservationRes.newBuilder().setState(state).build()
        lateinit var request: ZLinkSpotRequestCall
        request = publicStub { method, _ ->
            when (method) {
                "submit" ->
                    object :
                        CompletableFuture<Messages.GetBingoObservationRes>(),
                        ZLinkRequestStage<Messages.GetBingoObservationRes> {
                        init {
                            complete(observation)
                        }

                        override fun admitted() = CompletableFuture.completedFuture<Void>(null)
                    }
                else -> request
            }
        }
        val outbound =
            publicStub<ZLinkSpotOutbound> { method, args ->
                check(method == "requestToSpot")
                assertEquals("room", args[0])
                request
            }
        val entryContext =
            publicStub<ZLinkEntrySpotContext> { method, _ ->
                check(method == "outbound")
                outbound
            }
        var createdObserver = false
        val spots =
            publicStub<ZLinkSpotManager> { _, _ ->
                createdObserver = true
                error("A finished room must not create an observation room.")
            }
        val actorContext =
            publicStub<ZLinkActorContext> { _, _ -> error("Actor context is unused") }
        val entry = BingoEntrySpot(entryContext, spots)

        val response =
            entry.observeEvents(
                PlayerActor("observer", actorContext),
                ObserveBingoEventsReq("room"),
            )

        assertFalse(response.subscribed)
        assertFalse(createdObserver)
    }

    @Test
    fun playerJoinedNotificationFailureDoesNotRejectAdmissionOrStopRecipients() = runBlocking {
        val attempted = mutableListOf<String>()
        val delivered = mutableListOf<String>()
        val roomContext =
            publicStub<ZLinkSpotContext> { method, _ ->
                check(method == "spotId")
                "room"
            }
        val room =
            BingoRoomSpot(
                roomContext,
                BingoRoomSettingsInitializer(),
                publicStub<ZLinkRouteClient> { _, _ -> error("Routes are unused") },
            )
        room.applySettings(BingoRoomSettings.create("two-player", 0, 200).copy(requiredPlayers = 3))
        val failing = playerActor("disconnected", failPlayerJoined = true, attempted, delivered)
        val connected = playerActor("connected", failPlayerJoined = false, attempted, delivered)
        val newcomer = playerActor("new-player", failPlayerJoined = false, attempted, delivered)
        room.join(failing, BingoRoomJoinReq("room", "disconnected", "Disconnected", false), 0, 0)
        room.join(connected, BingoRoomJoinReq("room", "connected", "Connected", false), 0, 0)

        val response =
            room.join(newcomer, BingoRoomJoinReq("room", "new-player", "New", false), 0, 0)

        assertEquals(3, response.state.playersCount)
        assertEquals("Running", response.state.status)
        assertTrue(attempted.contains("disconnected"))
        assertTrue(attempted.contains("connected"))
        assertTrue(delivered.contains("connected"))
    }

    private fun playerActor(
        actorId: String,
        failPlayerJoined: Boolean,
        attempted: MutableList<String>,
        delivered: MutableList<String>,
    ): PlayerActor {
        var failCurrentNotification = false
        lateinit var sendCall: ZLinkBoundSessionSendCall
        sendCall = publicStub { method, _ ->
            when (method) {
                "metadata" -> sendCall
                "submit" -> {
                    if (failCurrentNotification && failPlayerJoined) {
                        CompletableFuture.failedFuture<Void>(
                            ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.UNAVAILABLE,
                                "The recipient session is closed.",
                            )
                        )
                    } else {
                        if (failCurrentNotification) delivered += actorId
                        CompletableFuture.completedFuture<Void>(null)
                    }
                }
                else -> error("Unexpected send call: $method")
            }
        }
        val session =
            publicStub<ZLinkBoundSession> { method, args ->
                check(method == "send")
                failCurrentNotification = args[0] is PlayerJoinedNotify
                if (failCurrentNotification) {
                    attempted += actorId
                }
                sendCall
            }
        val context =
            publicStub<ZLinkActorContext> { method, _ ->
                check(method == "boundSession")
                session
            }
        return PlayerActor(actorId, context)
    }

    private fun observe(failure: ZLinkFrameworkException? = null) = runBlocking {
        val pushed = mutableListOf<Any>()
        val reward =
            BingoRewardAcquiredEvent(
                "room",
                "winner",
                15,
                "rare-golden-dauber",
                "Golden Dauber",
                "Legendary",
            )
        val state =
            Messages.BingoRoomState.newBuilder()
                .setRoomId("room")
                .setStatus("Finished")
                .setDrawSeq(15)
                .addWinners("winner")
                .build()
        val observation =
            Messages.GetBingoObservationRes.newBuilder().setState(state).setReward(reward).build()
        val request =
            publicStub<ZLinkSpotRequestCall> { method, _ ->
                when (method) {
                    "inMesh" -> error("inMesh is supplied below")
                    "submit" ->
                        object :
                            CompletableFuture<Messages.GetBingoObservationRes>(),
                            ZLinkRequestStage<Messages.GetBingoObservationRes> {
                            init {
                                if (failure == null) complete(observation)
                                else completeExceptionally(failure)
                            }

                            override fun admitted() = CompletableFuture.completedFuture<Void>(null)
                        }
                    else -> error("Unexpected request call: $method")
                }
            }
        val outbound =
            publicStub<ZLinkSpotOutbound> { method, args ->
                check(method == "requestToSpot")
                assertEquals("room", args[0])
                request
            }
        val context =
            publicStub<ZLinkSpotContext> { method, _ ->
                when (method) {
                    "spotId" -> "observe:room:observer"
                    "outbound" -> outbound
                    "relocationReady" -> ZLinkSpotRelocationReadyCall {}
                    else -> error("Unexpected Spot call: $method")
                }
            }
        val session =
            publicStub<ZLinkBoundSession> { method, args ->
                check(method == "send")
                pushed += args[0]!!
                lateinit var send: ZLinkBoundSessionSendCall
                send = publicStub { call, _ ->
                    when (call) {
                        "metadata" -> send
                        "submit" -> CompletableFuture.completedFuture<Void>(null)
                        else -> error("Unexpected send: $call")
                    }
                }
                send
            }
        val actorContext =
            publicStub<ZLinkActorContext> { method, _ ->
                when (method) {
                    "actorId" -> "observer"
                    "boundSession" -> session
                    else -> error("Unexpected Actor call: $method")
                }
            }
        val room =
            BingoRoomSpot(
                context,
                BingoRoomSettingsInitializer(),
                publicStub<ZLinkRouteClient> { _, _ -> error("Unused route") },
            )
        room.applySettings(BingoRoomSettings.createObserver("room", "observer", 200))
        val actor = PlayerActor("observer", actorContext)
        room
            .onActorJoin(
                "observer",
                ZLinkMessage.of(BingoRoomJoinReq("room", "observer", "Observer", true)),
            )
            .toCompletableFuture()
            .join()
        val callback = room.onJoinedActor(actor).toCompletableFuture()
        if (failure?.kind() == ZLinkFrameworkErrorKind.UNAVAILABLE) {
            assertSame(
                failure,
                assertThrows(java.util.concurrent.CompletionException::class.java) {
                        callback.join()
                    }
                    .cause,
            )
            assertTrue(pushed.isEmpty())
            return@runBlocking
        }
        callback.join()
        if (failure != null) {
            assertTrue(pushed.isEmpty())
            return@runBlocking
        }
        assertEquals(1, pushed.size)
        assertEquals(state, (pushed[0] as Messages.BingoObservationStateNotify).state)
        assertTrue(pushed.none { it is BingoRewardAnnouncedNotify })
        room.announceReward(reward)
        assertEquals(2, pushed.size)
    }
}
