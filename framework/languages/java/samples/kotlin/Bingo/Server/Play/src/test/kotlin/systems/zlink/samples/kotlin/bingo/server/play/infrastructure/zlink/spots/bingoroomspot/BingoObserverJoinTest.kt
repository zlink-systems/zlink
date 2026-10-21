package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot

import java.lang.reflect.Proxy
import java.util.concurrent.CompletableFuture
import kotlinx.coroutines.runBlocking
import org.junit.jupiter.api.Assertions.*
import org.junit.jupiter.api.Test
import systems.zlink.framework.actors.*
import systems.zlink.framework.channels.*
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.*
import systems.zlink.samples.kotlin.bingo.server.play.domain.bingo.BingoRoomSettings
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors.PlayerActor
import systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.spots.bingoroomspot.handlers.BingoRoomSettingsInitializer
import systems.zlink.samples.kotlin.bingo.shared.contracts.*

class BingoObserverJoinTest {
    private inline fun <reified T> publicStub(
        crossinline call: (String, Array<out Any?>) -> Any?
    ): T =
        Proxy.newProxyInstance(T::class.java.classLoader, arrayOf(T::class.java)) { _, method, args
            ->
            call(method.name, args ?: emptyArray())
        } as T

    @Test
    fun finishedRoomRewardIsDeliveredAfterObserverMembershipCommits() = runBlocking {
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
                                complete(observation)
                            }

                            override fun admitted() = CompletableFuture.completedFuture<Void>(null)
                        }
                    else -> error("Unexpected request call: $method")
                }
            }
        val routed =
            publicStub<ZLinkSpotRequestCall> { method, _ ->
                check(method == "inMesh")
                request
            }
        val outbound =
            publicStub<ZLinkSpotOutbound> { method, args ->
                check(method == "requestToSpot")
                assertEquals("room", args[0])
                routed
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
        room.onJoinedActor(actor).toCompletableFuture().join()
        assertEquals(2, pushed.size)
        assertEquals(state, (pushed[0] as Messages.BingoObservationStateNotify).state)
        assertEquals("winner", (pushed[1] as BingoRewardAnnouncedNotify).actorId)
        room.announceReward(reward)
        assertEquals(2, pushed.size)
    }
}
