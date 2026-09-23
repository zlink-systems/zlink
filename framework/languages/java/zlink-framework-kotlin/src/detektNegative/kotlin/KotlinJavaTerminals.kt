package detektnegative

import java.time.Duration
import systems.zlink.framework.ZLinkHandlerFilter
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorFactory
import systems.zlink.framework.channels.ZLinkRequestHandler
import systems.zlink.framework.channels.ZLinkRouteSendHandler
import systems.zlink.framework.channels.ZLinkSendHandler
import systems.zlink.framework.configuration.FanoutChannelBuilder
import systems.zlink.framework.configuration.ZLinkActorFactoryBuilder
import systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder
import systems.zlink.framework.configuration.ZLinkFrameworkOptions
import systems.zlink.framework.configuration.ZLinkInstanceSpotFactoryBuilder
import systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder
import systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder
import systems.zlink.framework.configuration.ZLinkStreamNodeBuilder
import systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkEntrySpot
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.spots.ZLinkInstanceSpot
import systems.zlink.framework.spots.ZLinkInstanceSpotContext
import systems.zlink.framework.spots.ZLinkInstanceSpotHandlerRegistry
import systems.zlink.framework.spots.ZLinkSpot
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry
import systems.zlink.framework.streams.ZLinkSession
import systems.zlink.framework.streams.ZLinkSessionActor
import systems.zlink.framework.streams.ZLinkSessionDispatchContext

@Suppress("UNUSED_PARAMETER")
fun forbiddenRegistrationOverloads(
    options: ZLinkFrameworkOptions,
    objectServer: ZLinkMeshObjectServerBuilder,
    actorFactory: ZLinkActorFactoryBuilder<*>,
    userSpotFactory: ZLinkUserSpotFactoryBuilder<*>,
    instanceSpotFactory: ZLinkInstanceSpotFactoryBuilder<*>,
    fanout: FanoutChannelBuilder,
    streamNode: ZLinkStreamNodeBuilder,
    meshNode: ZLinkMeshNodeBuilder,
    meshChannel: ZLinkMeshChannelServerBuilder,
    clientServer: ZLinkClientServerChannelServerBuilder,
    spotHandlers: ZLinkSpotHandlerRegistry,
    instanceSpotHandlers: ZLinkInstanceSpotHandlerRegistry,
    entrySpot: ZLinkEntrySpotContext,
    spot: ZLinkSpotContext,
    instanceSpot: ZLinkInstanceSpotContext,
) {
    options.addHandlersFromPackageOf(String::class.java)
    options.useFilter(type<ZLinkHandlerFilter>())
    objectServer.addEntrySpot(type<ZLinkEntrySpot<*>>())
    objectServer.addSpotFactory("spot", type<ZLinkSpot<*>>()) {}
    objectServer.addInstanceSpotFactory("instance", type<ZLinkInstanceSpot>()) {}
    objectServer.addActorFactory("actor", type<ZLinkActor>(), type<ZLinkActorFactory>()) {}
    actorFactory.preserveStateWith(fixture())
    userSpotFactory.preserveStateWith(fixture())
    instanceSpotFactory.preserveStateWith(fixture())
    fanout.addPublishHandler(String::class.java, String::class.java)
    fanout.addPublishHandler(String::class.java, String::class.java, "packet")
    fanout.addPublishHandler(String::class.java)
    fanout.addPublishHandler(String::class.java, "packet")
    streamNode.registerSession(type<ZLinkSession>())
    streamNode.addSessionPacketHandler(String::class.java)
    meshNode.addRouteSendHandler(String::class.java, String::class.java)
    meshNode.addRouteRequestHandler(String::class.java, String::class.java, String::class.java)
    meshChannel.addSendHandler(type<ZLinkSendHandler<String>>(), String::class.java)
    meshChannel.addRouteSendHandler(type<ZLinkRouteSendHandler<String>>(), String::class.java)
    meshChannel.addRequestHandler(
        type<ZLinkRequestHandler<String, String>>(),
        String::class.java,
        String::class.java,
    )
    clientServer.addSendHandler(type<ZLinkSendHandler<String>>(), String::class.java)
    clientServer.addRequestHandler(
        type<ZLinkRequestHandler<String, String>>(),
        String::class.java,
        String::class.java,
    )
    spotHandlers.addHandler(String::class.java)
    instanceSpotHandlers.addPacket(String::class.java)
    entrySpot.addTimer("timer", Duration.ZERO, String::class.java, fixture())
    spot.addTimer("timer", Duration.ZERO, String::class.java, fixture())
    instanceSpot.addTimer("timer", Duration.ZERO, String::class.java, fixture())
}

fun forbiddenSessionActorOverloads(
    actor: ZLinkSessionActor,
    dispatch: ZLinkSessionDispatchContext,
    message: ZLinkMessage,
) {
    actor.relay(message)
    actor.relay(dispatch, message)
}

private fun <T> fixture(): T = error("negative Detekt fixture is not executed")

@Suppress("UNCHECKED_CAST") private fun <T> type(): Class<T> = String::class.java as Class<T>
