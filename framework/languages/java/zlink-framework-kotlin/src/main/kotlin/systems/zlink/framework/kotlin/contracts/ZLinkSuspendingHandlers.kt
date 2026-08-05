package systems.zlink.framework.kotlin

import java.util.concurrent.CompletionStage
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.future.future
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorFactory
import systems.zlink.framework.actors.ZLinkActorJoinCompletion
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.channels.ZLinkPublishMessageContext
import systems.zlink.framework.channels.ZLinkRouteMessageContext
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.ZLinkEntrySpot
import systems.zlink.framework.spots.ZLinkInstanceSpot
import systems.zlink.framework.spots.ZLinkInstanceSpotContext
import systems.zlink.framework.spots.ZLinkSpot
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult
import systems.zlink.framework.spots.ZLinkActorCreateResponse
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkSpotClosingContext
import systems.zlink.framework.spots.ZLinkSpotCreateResponse
import systems.zlink.framework.spots.ZLinkSpotRelocationReadyCompletion
import systems.zlink.framework.spots.ZLinkTimerTick
import systems.zlink.framework.streams.ZLinkSession
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkStreamError
import systems.zlink.framework.streams.ZLinkSessionDispatchContext

interface ZLinkSuspendingRequestHandler<TRequest, TReply> {
    suspend fun handle(request: TRequest, context: ZLinkMessageContext): TReply
}

interface ZLinkSuspendingSendHandler<TMessage> {
    suspend fun handle(message: TMessage, context: ZLinkMessageContext)
}

interface ZLinkSuspendingPublishHandler<TMessage> {
    suspend fun handle(message: TMessage, context: ZLinkPublishMessageContext)
}

interface ZLinkSuspendingRouteRequestHandler<TRequest, TReply> {
    suspend fun handle(request: TRequest, context: ZLinkRouteMessageContext): TReply
}

interface ZLinkSuspendingRouteSendHandler<TMessage> {
    suspend fun handle(message: TMessage, context: ZLinkRouteMessageContext)
}

interface ZLinkSuspendingSpotPacketHandler<TSpot : ZLinkSpot<*>, TMessage> {
    suspend fun handle(spot: TSpot, message: TMessage)

    suspend fun handle(spot: TSpot, message: TMessage, context: ZLinkMessageContext) =
        handle(spot, message)
}

interface ZLinkSuspendingSpotRequestHandler<TSpot : Any, TRequest, TReply> {
    suspend fun handle(spot: TSpot, request: TRequest): TReply

    suspend fun handle(
        spot: TSpot,
        request: TRequest,
        context: ZLinkMessageContext,
    ): TReply = handle(spot, request)
}

interface ZLinkSuspendingSpotSubscriptionHandler<TSpot : Any, TEvent> {
    suspend fun handle(spot: TSpot, event: TEvent)

    suspend fun handle(
        spot: TSpot,
        event: TEvent,
        context: ZLinkPublishMessageContext,
    ) = handle(spot, event)
}

interface ZLinkSuspendingSpotTimerHandler<TSpot : Any> {
    suspend fun handle(spot: TSpot, tick: ZLinkTimerTick)
}

interface ZLinkSuspendingEntrySpotActorSendHandler<
    TEntrySpot : ZLinkEntrySpot<*>,
    TActor : ZLinkActor,
    TMessage,
> {
    suspend fun handle(
        entrySpot: TEntrySpot,
        actor: TActor,
        context: ZLinkMessageContext,
        message: TMessage,
    )
}

interface ZLinkSuspendingEntrySpotActorRequestHandler<
    TEntrySpot : ZLinkEntrySpot<*>,
    TActor : ZLinkActor,
    TRequest,
    TReply,
> {
    suspend fun handle(
        entrySpot: TEntrySpot,
        actor: TActor,
        context: ZLinkMessageContext,
        request: TRequest,
    ): TReply
}

interface ZLinkSuspendingSpotActorSendHandler<
    TSpot : ZLinkSpot<*>,
    TActor : ZLinkActor,
    TMessage,
> {
    suspend fun handle(
        spot: TSpot,
        actor: TActor,
        context: ZLinkMessageContext,
        message: TMessage,
    )
}

interface ZLinkSuspendingSpotActorRequestHandler<
    TSpot : ZLinkSpot<*>,
    TActor : ZLinkActor,
    TRequest,
    TReply,
> {
    suspend fun handle(
        spot: TSpot,
        actor: TActor,
        context: ZLinkMessageContext,
        request: TRequest,
    ): TReply
}

interface ZLinkSuspendingTypedSessionPacketHandler<
    TSessionContext : ZLinkSessionContext,
    TMessage : Any,
> {
    fun packetName(): String

    fun messageType(): Class<TMessage>

    suspend fun handle(context: TSessionContext, dispatch: ZLinkSessionDispatchContext, message: TMessage)
}

abstract class ZLinkSuspendingActorFactory : ZLinkActorFactory {
    final override fun create(context: ZLinkActorContext): CompletionStage<ZLinkActor> =
        coroutineStage {
            createActor(context)
        }

    protected abstract suspend fun createActor(context: ZLinkActorContext): ZLinkActor
}

abstract class ZLinkSuspendingActor : ZLinkActor {
    abstract val context: ZLinkActorContext

    final override fun context(): ZLinkActorContext = context

    final override fun onJoinCompleted(
        completion: ZLinkActorJoinCompletion,
    ): CompletionStage<Void> = coroutineVoidStage {
        onJoinCompletedSuspending(completion)
    }

    abstract suspend fun onJoinCompletedSuspending(
        completion: ZLinkActorJoinCompletion,
    )
}

abstract class ZLinkSuspendingSpot<TActor : ZLinkActor> : ZLinkSpot<TActor> {
    abstract val context: ZLinkSpotContext

    final override fun context(): ZLinkSpotContext = context

    final override fun onCreate(request: ZLinkMessage): CompletionStage<ZLinkSpotCreateResponse> =
        coroutineStage { onCreateSuspending(request) }

    final override fun onInitialize(): CompletionStage<Void> =
        coroutineVoidStage { onInitializeSuspending() }

    final override fun onClosing(
        context: ZLinkSpotClosingContext,
    ): CompletionStage<Void> = coroutineVoidStage {
        onClosingSuspending(context)
    }

    final override fun onRelocationReadyCompleted(
        completion: ZLinkSpotRelocationReadyCompletion,
    ): CompletionStage<Void> = coroutineVoidStage {
        onRelocationReadyCompletedSuspending(completion)
    }

    final override fun onActorJoin(actorId: String, request: ZLinkMessage): CompletionStage<ZLinkSpotActorJoinResult> =
        coroutineStage { onActorJoinSuspending(actorId, request) }

    final override fun onJoinedActor(actor: TActor): CompletionStage<Void> =
        coroutineVoidStage { onJoinedActorSuspending(actor) }

    final override fun onLeaveActor(actor: TActor): CompletionStage<Void> =
        coroutineVoidStage { onLeaveActorSuspending(actor) }

    final override fun onDisconnectActor(actor: TActor): CompletionStage<Void> =
        coroutineVoidStage { onDisconnectActorSuspending(actor) }

    protected open suspend fun onCreateSuspending(request: ZLinkMessage): ZLinkSpotCreateResponse =
        ZLinkSpotCreateResponse.accept()

    protected open suspend fun onInitializeSuspending() {
    }

    protected open suspend fun onClosingSuspending(
        context: ZLinkSpotClosingContext,
    ) {
    }

    protected open suspend fun onRelocationReadyCompletedSuspending(
        completion: ZLinkSpotRelocationReadyCompletion,
    ) {
    }

    protected abstract suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult

    protected abstract suspend fun onJoinedActorSuspending(actor: TActor)

    protected abstract suspend fun onLeaveActorSuspending(actor: TActor)

    protected open suspend fun onDisconnectActorSuspending(actor: TActor) {
    }
}

abstract class ZLinkSuspendingEntrySpot<TActor : ZLinkActor> : ZLinkEntrySpot<TActor> {
    abstract val context: systems.zlink.framework.spots.ZLinkEntrySpotContext

    final override fun context(): systems.zlink.framework.spots.ZLinkEntrySpotContext = context

    final override fun onInitialize(): CompletionStage<Void> = coroutineVoidStage { onInitializeSuspending() }

    final override fun onClosing(
        context: ZLinkSpotClosingContext,
    ): CompletionStage<Void> = coroutineVoidStage {
        onClosingSuspending(context)
    }

    final override fun onCreateActor(
        actor: TActor,
        createRequest: ZLinkMessage,
    ): CompletionStage<ZLinkActorCreateResponse> =
        coroutineStage { onCreateActorSuspending(actor, createRequest) }

    final override fun onJoinedActor(actor: TActor): CompletionStage<Void> =
        coroutineVoidStage { onJoinedActorSuspending(actor) }

    final override fun onLeaveActor(actor: TActor): CompletionStage<Void> =
        coroutineVoidStage { onLeaveActorSuspending(actor) }

    final override fun onDisconnectActor(actor: TActor): CompletionStage<Void> =
        coroutineVoidStage { onDisconnectActorSuspending(actor) }

    protected open suspend fun onInitializeSuspending() {
    }

    protected open suspend fun onClosingSuspending(
        context: ZLinkSpotClosingContext,
    ) {
    }

    protected open suspend fun onCreateActorSuspending(
        actor: TActor,
        createRequest: ZLinkMessage,
    ): ZLinkActorCreateResponse = ZLinkActorCreateResponse.accept()

    protected abstract suspend fun onJoinedActorSuspending(actor: TActor)

    protected abstract suspend fun onLeaveActorSuspending(actor: TActor)

    protected open suspend fun onDisconnectActorSuspending(actor: TActor) {
    }
}

/** Coroutine lifecycle adapter for an Instance Spot. */
abstract class ZLinkSuspendingInstanceSpot : ZLinkInstanceSpot {
    abstract val context: ZLinkInstanceSpotContext

    final override fun context(): ZLinkInstanceSpotContext = context

    final override fun onInitialize(): CompletionStage<Void> =
        coroutineVoidStage { onInitializeSuspending() }

    final override fun onClosing(
        context: ZLinkSpotClosingContext,
    ): CompletionStage<Void> = coroutineVoidStage {
        onClosingSuspending(context)
    }

    protected open suspend fun onInitializeSuspending() {
    }

    protected open suspend fun onClosingSuspending(
        context: ZLinkSpotClosingContext,
    ) {
    }
}

abstract class ZLinkSuspendingSession : ZLinkSession {
    abstract override fun context(): ZLinkSessionContext

    final override fun onConnected(): CompletionStage<Void> = coroutineVoidStage { onConnectedSuspending() }

    final override fun onDisconnected(): CompletionStage<Void> = coroutineVoidStage { onDisconnectedSuspending() }

    final override fun onError(error: ZLinkStreamError): CompletionStage<Void> =
        coroutineVoidStage { onErrorSuspending(error) }

    final override fun onDispatch(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ): CompletionStage<Void> = coroutineVoidStage { onDispatchSuspending(dispatch, payload) }

    protected open suspend fun onConnectedSuspending() {
    }

    protected open suspend fun onDisconnectedSuspending() {
    }

    protected open suspend fun onErrorSuspending(error: ZLinkStreamError) {
    }

    protected open suspend fun onDispatchSuspending(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage) {
    }
}

private val bridgeScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

private fun <T> coroutineStage(block: suspend () -> T): CompletionStage<T> =
    bridgeScope.future(ZLinkCoroutineInvocationContext.capture(Dispatchers.Default)) { block() }

private fun coroutineVoidStage(block: suspend () -> Unit): CompletionStage<Void> =
    bridgeScope.future(ZLinkCoroutineInvocationContext.capture(Dispatchers.Default)) {
        block()
        null
    }
