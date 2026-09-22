package systems.zlink.framework.kotlin

import systems.zlink.contracts.core.RoutingId
import systems.zlink.contracts.messaging.Message
import systems.zlink.framework.ZLinkHandlerFilter
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ActorRefSnapshot
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.actors.ZLinkActorDirectory
import systems.zlink.framework.actors.ZLinkActorFactory
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter
import systems.zlink.framework.actors.ZLinkActorRequestCall
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.channels.ZLinkFanoutClient
import systems.zlink.framework.channels.ZLinkFanoutPublishCall
import systems.zlink.framework.channels.ZLinkRequestCall
import systems.zlink.framework.channels.ZLinkRequestHandler
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.channels.ZLinkRouteSendHandler
import systems.zlink.framework.channels.ZLinkSendCall
import systems.zlink.framework.channels.ZLinkSendHandler
import systems.zlink.framework.configuration.FanoutChannelBuilder
import systems.zlink.framework.configuration.ZLinkActorFactoryBuilder
import systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder
import systems.zlink.framework.configuration.ZLinkFrameworkOptions
import systems.zlink.framework.configuration.ZLinkInstanceSpotFactoryBuilder
import systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder
import systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder
import systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder
import systems.zlink.framework.configuration.ZLinkStreamNodeBuilder
import systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder
import systems.zlink.framework.locations.ZLinkLocationReadiness
import systems.zlink.framework.locations.ZLinkLocationRole
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkEntrySpot
import systems.zlink.framework.spots.ZLinkInstanceSpot
import systems.zlink.framework.spots.ZLinkSpot
import systems.zlink.framework.spots.ZLinkSpotRelocationAdapter
import systems.zlink.framework.spots.ZLinkWorkerCall
import systems.zlink.framework.streams.ZLinkSession
import systems.zlink.framework.streams.ZLinkSessionActor
import systems.zlink.framework.streams.ZLinkSessionActors

suspend fun <TReply> ZLinkRequestCall.awaitReply(replyType: Class<TReply>): TReply =
    awaitFrameworkStage(submit(replyType))

inline suspend fun <reified TReply> ZLinkRequestCall.awaitReply(): TReply =
    awaitReply(TReply::class.java)

suspend fun <TReply> ZLinkRequestCall.yieldReply(replyType: Class<TReply>): TReply =
    awaitFrameworkStage(yield(replyType))

inline suspend fun <reified TReply> ZLinkRequestCall.yieldReply(): TReply =
    yieldReply(TReply::class.java)

suspend fun <TReply> ZLinkActorRequestCall.awaitReply(replyType: Class<TReply>): TReply =
    awaitFrameworkStage(submit(replyType))

inline suspend fun <reified TReply> ZLinkActorRequestCall.awaitReply(): TReply =
    awaitReply(TReply::class.java)

suspend fun <TReply> ZLinkActorRequestCall.yieldReply(replyType: Class<TReply>): TReply =
    awaitFrameworkStage(yield(replyType))

inline suspend fun <reified TReply> ZLinkActorRequestCall.yieldReply(): TReply =
    yieldReply(TReply::class.java)

suspend fun <TReply> ZLinkActorClient.requestToActorAwait(
    actorId: String,
    request: Any,
    replyType: Class<TReply>,
): TReply = requestToActor(actorId, request).awaitReply(replyType)

inline suspend fun <reified TReply> ZLinkActorClient.requestToActorAwait(
    actorId: String,
    request: Any,
): TReply = requestToActorAwait(actorId, request, TReply::class.java)

suspend fun ZLinkActorDirectory.findActor(actorId: String): ActorRef? =
    awaitFrameworkStage(find(actorId)).orElse(null)

suspend fun ZLinkActorDirectory.ensureActor(
    actorId: String,
    createRequest: ZLinkMessage,
): ActorRef = awaitFrameworkStage(ensure(actorId, createRequest))

suspend fun ZLinkActorDirectory.ensureActor(actorId: String, createRequest: Any): ActorRef =
    awaitFrameworkStage(ensure(actorId, createRequest))

fun ActorRef.snapshot(): ActorRefSnapshot = ActorRefSnapshot.from(this)

fun ActorRefSnapshot.actorRef(): ActorRef = toActorRef()

suspend fun ZLinkLocationReadiness.isPeerReady(
    meshName: String,
    role: ZLinkLocationRole,
    nodeRid: RoutingId? = null,
): Boolean = awaitFrameworkStage(isPeerReady(meshName, role, nodeRid))

suspend fun ZLinkSessionActors.bindOrGetActor(actor: ActorRef): ZLinkSessionActor =
    awaitFrameworkStage(bindOrGet(actor))

suspend fun <T> ZLinkWorkerCall<T>.yieldWorker(): T = awaitFrameworkStage(yield())

fun <TMessage> ZLinkClient.send(channelName: String, message: TMessage): ZLinkSendCall =
    sendToChannel(channelName, message)

suspend inline fun <reified TReply> ZLinkClient.request(
    channelName: String,
    message: Message,
): TReply = requestToChannel(channelName, message).awaitReply()

fun <TEvent> ZLinkFanoutClient.publishToTopic(
    channelName: String,
    message: TEvent,
): ZLinkFanoutPublishCall = publish(channelName, message)

fun <TMessage> ZLinkRouteClient.send(
    meshName: String,
    target: RoutingId,
    message: TMessage,
): ZLinkSendCall = sendToNode(meshName, target, message)

fun <TMessage> ZLinkRouteClient.send(channelName: String, message: TMessage): ZLinkSendCall =
    sendToChannel(channelName, message)

suspend inline fun <reified TReply> ZLinkRouteClient.request(
    channelName: String,
    target: RoutingId,
    message: Message,
): TReply = requestToNode(channelName, target, message).awaitReply()

fun <TMessage> ZLinkRouteClient.sendToSpotCall(spotId: String, message: TMessage): ZLinkSendCall =
    sendToSpot(spotId, message)

suspend inline fun <reified TReply> ZLinkRouteClient.requestToSpotAwait(
    spotId: String,
    message: Message,
): TReply = requestToSpot(spotId, message).awaitReply()

fun ZLinkFrameworkOptions.configureStreamCompression(
    configure: ZLinkStreamCompressionBuilder.() -> Unit
): ZLinkFrameworkOptions {
    configureStreamCompression().configure()
    return this
}

inline fun <
    reified TActor : ZLinkActor,
    reified TFactory : ZLinkActorFactory,
> ZLinkMeshObjectServerBuilder.addActorFactory(
    actorType: String,
    noinline configure: ZLinkActorFactoryBuilder<TActor>.() -> Unit,
): ZLinkMeshObjectServerBuilder =
    addActorFactory(actorType, TActor::class.java, TFactory::class.java) { factory ->
        factory.configure()
    }

inline fun <reified TMarker : Any> ZLinkFrameworkOptions.addHandlersFromPackageOf() =
    addHandlersFromPackageOf(TMarker::class.java)

inline fun <reified TFilter : ZLinkHandlerFilter> ZLinkFrameworkOptions.useFilter() =
    useFilter(TFilter::class.java)

inline fun <reified TEntrySpot : ZLinkEntrySpot<*>> ZLinkMeshObjectServerBuilder.addEntrySpot():
    ZLinkMeshObjectServerBuilder = addEntrySpot(TEntrySpot::class.java)

inline fun <reified TSpot : ZLinkSpot<*>> ZLinkMeshObjectServerBuilder.addSpotFactory(
    stableType: String,
    noinline configure: ZLinkUserSpotFactoryBuilder<TSpot>.() -> Unit,
): ZLinkMeshObjectServerBuilder = addSpotFactory(stableType, TSpot::class.java) { it.configure() }

inline fun <reified TSpot : ZLinkInstanceSpot> ZLinkMeshObjectServerBuilder.addInstanceSpotFactory(
    stableType: String,
    noinline configure: ZLinkInstanceSpotFactoryBuilder<TSpot>.() -> Unit,
): ZLinkMeshObjectServerBuilder =
    addInstanceSpotFactory(stableType, TSpot::class.java) { it.configure() }

inline fun <
    TActor : ZLinkActor,
    reified TAdapter : ZLinkActorRelocationAdapter<TActor>,
> ZLinkActorFactoryBuilder<TActor>.preserveStateWith() = preserveStateWith(TAdapter::class.java)

inline fun <
    TSpot : ZLinkSpot<*>,
    reified TAdapter : ZLinkSpotRelocationAdapter<TSpot>,
> ZLinkUserSpotFactoryBuilder<TSpot>.preserveStateWith() = preserveStateWith(TAdapter::class.java)

inline fun <
    TSpot : ZLinkInstanceSpot,
    reified TAdapter : ZLinkSpotRelocationAdapter<TSpot>,
> ZLinkInstanceSpotFactoryBuilder<TSpot>.preserveStateWith() =
    preserveStateWith(TAdapter::class.java)

@JvmName("addPublishHandlerForMessage")
inline fun <reified THandler : Any, reified TMessage : Any> FanoutChannelBuilder
    .addPublishHandler() = addPublishHandler(THandler::class.java, TMessage::class.java)

@JvmName("addPublishHandlerForMessageAndPacketName")
inline fun <reified THandler : Any, reified TMessage : Any> FanoutChannelBuilder.addPublishHandler(
    packetName: String
) = addPublishHandler(THandler::class.java, TMessage::class.java, packetName)

inline fun <reified THandler : Any> FanoutChannelBuilder.addPublishHandler(): FanoutChannelBuilder =
    addPublishHandler(THandler::class.java)

inline fun <reified THandler : Any> FanoutChannelBuilder.addPublishHandler(
    packetName: String
): FanoutChannelBuilder = addPublishHandler(THandler::class.java, packetName)

inline fun <reified TSession : ZLinkSession> ZLinkStreamNodeBuilder.registerSession():
    ZLinkStreamNodeBuilder = registerSession(TSession::class.java)

inline fun <reified THandler : Any> ZLinkStreamNodeBuilder.addSessionPacketHandler():
    ZLinkStreamNodeBuilder = addSessionPacketHandler(THandler::class.java)

inline fun <reified THandler : Any, reified TMessage : Any> ZLinkMeshNodeBuilder
    .addRouteSendHandler(): ZLinkMeshNodeBuilder =
    addRouteSendHandler(THandler::class.java, TMessage::class.java)

inline fun <
    reified THandler : Any,
    reified TRequest : Any,
    reified TReply : Any,
> ZLinkMeshNodeBuilder.addRouteRequestHandler(): ZLinkMeshNodeBuilder =
    addRouteRequestHandler(THandler::class.java, TRequest::class.java, TReply::class.java)

inline fun <
    reified THandler : ZLinkSendHandler<TMessage>,
    reified TMessage : Any,
> ZLinkMeshChannelServerBuilder.addSendHandler(): ZLinkMeshChannelServerBuilder =
    addSendHandler(THandler::class.java, TMessage::class.java)

inline fun <
    reified THandler : ZLinkRouteSendHandler<TMessage>,
    reified TMessage : Any,
> ZLinkMeshChannelServerBuilder.addRouteSendHandler(): ZLinkMeshChannelServerBuilder =
    addRouteSendHandler(THandler::class.java, TMessage::class.java)

inline fun <
    reified THandler : ZLinkRequestHandler<TRequest, TReply>,
    reified TRequest : Any,
    reified TReply : Any,
> ZLinkMeshChannelServerBuilder.addRequestHandler(): ZLinkMeshChannelServerBuilder =
    addRequestHandler(THandler::class.java, TRequest::class.java, TReply::class.java)

inline fun <
    reified THandler : ZLinkSendHandler<TMessage>,
    reified TMessage : Any,
> ZLinkClientServerChannelServerBuilder.addSendHandler(): ZLinkClientServerChannelServerBuilder =
    addSendHandler(THandler::class.java, TMessage::class.java)

inline fun <
    reified THandler : ZLinkRequestHandler<TRequest, TReply>,
    reified TRequest : Any,
    reified TReply : Any,
> ZLinkClientServerChannelServerBuilder.addRequestHandler(): ZLinkClientServerChannelServerBuilder =
    addRequestHandler(THandler::class.java, TRequest::class.java, TReply::class.java)
