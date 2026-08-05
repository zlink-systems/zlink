package systems.zlink.framework.kotlin

import java.time.Duration
import kotlin.reflect.KClass
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.spots.ZLinkSpotCreateResult

/**
 * Kotlin one-way terminal. Successful completion means local queue admission;
 * delivery and handler completion are outside this result boundary.
 */
interface ZLinkKotlinMessageSendCall {
    fun metadata(key: String, value: String): ZLinkKotlinMessageSendCall
    suspend fun await()
}

interface ZLinkKotlinSubmissionCall {
    suspend fun await()
}

interface ZLinkKotlinRequestCall<TReply : Any> {
    fun metadata(key: String, value: String): ZLinkKotlinRequestCall<TReply>
    fun timeout(timeout: Duration): ZLinkKotlinRequestCall<TReply>
    suspend fun await(): TReply
    suspend fun yield(): TReply
}

interface ZLinkKotlinClient {
    fun sendToChannel(channelName: String, message: Any): ZLinkKotlinMessageSendCall

    fun <TReply : Any> requestToChannel(
        channelName: String,
        request: Any,
        replyType: KClass<TReply>,
    ): ZLinkKotlinRequestCall<TReply>
}

interface ZLinkKotlinFanoutClient {
    fun publish(
        channelName: String,
        topic: String,
        event: Any,
    ): ZLinkKotlinSubmissionCall

    fun publish(channelName: String, event: Any): ZLinkKotlinSubmissionCall
}

interface ZLinkKotlinRouteClient {
    fun sendToNode(
        meshName: String,
        target: RoutingId,
        message: Any,
    ): ZLinkKotlinMessageSendCall

    fun <TReply : Any> requestToNode(
        meshName: String,
        target: RoutingId,
        request: Any,
        replyType: KClass<TReply>,
    ): ZLinkKotlinRequestCall<TReply>

    fun sendToChannel(channelName: String, message: Any): ZLinkKotlinMessageSendCall

    fun <TReply : Any> requestToChannel(
        channelName: String,
        request: Any,
        replyType: KClass<TReply>,
    ): ZLinkKotlinRequestCall<TReply>
}

interface ZLinkKotlinActorClient {
    fun sendToActor(actorId: String, message: Any): ZLinkKotlinMessageSendCall

    fun <TReply : Any> requestToActor(
        actorId: String,
        request: Any,
        replyType: KClass<TReply>,
    ): ZLinkKotlinRequestCall<TReply>
}

interface ZLinkKotlinActorCreateCall {
    fun inMesh(meshName: String): ZLinkKotlinActorCreateCall
    fun request(request: Any): ZLinkKotlinActorCreateCall
    fun timeout(timeout: Duration): ZLinkKotlinActorCreateCall
    suspend fun await(): ZLinkActorCreateResult
    suspend fun yield(): ZLinkActorCreateResult
}

interface ZLinkKotlinActorManager {
    fun create(actorId: String, actorType: String): ZLinkKotlinActorCreateCall
    fun getOrCreate(actorId: String, actorType: String): ZLinkKotlinActorCreateCall
}

interface ZLinkKotlinSpotSendCall {
    fun metadata(key: String, value: String): ZLinkKotlinSpotSendCall
    fun instanceSpot(): ZLinkKotlinSpotSendCall
    fun instanceSpot(stableType: String): ZLinkKotlinSpotSendCall
    fun inMesh(meshName: String): ZLinkKotlinSpotSendCall
    suspend fun await()
}

interface ZLinkKotlinSpotRequestCall<TReply : Any> {
    fun metadata(key: String, value: String): ZLinkKotlinSpotRequestCall<TReply>
    fun instanceSpot(): ZLinkKotlinSpotRequestCall<TReply>
    fun instanceSpot(stableType: String): ZLinkKotlinSpotRequestCall<TReply>
    fun inMesh(meshName: String): ZLinkKotlinSpotRequestCall<TReply>
    fun timeout(timeout: Duration): ZLinkKotlinSpotRequestCall<TReply>
    suspend fun await(): TReply
    suspend fun yield(): TReply
}

interface ZLinkKotlinSpotCreateCall {
    fun inMesh(meshName: String): ZLinkKotlinSpotCreateCall
    fun request(request: Any): ZLinkKotlinSpotCreateCall
    fun timeout(timeout: Duration): ZLinkKotlinSpotCreateCall
    suspend fun await(): ZLinkSpotCreateResult
    suspend fun yield(): ZLinkSpotCreateResult
}

interface ZLinkKotlinSpotManager {
    fun create(stableType: String): ZLinkKotlinSpotCreateCall
    fun getOrCreate(spotId: String, stableType: String): ZLinkKotlinSpotCreateCall
}

interface ZLinkKotlinSessionSendCall {
    fun metadata(key: String, value: String): ZLinkKotlinSessionSendCall
    fun compress(): ZLinkKotlinSessionSendCall
    suspend fun await()
}

interface ZLinkKotlinSessionReplyCall {
    fun compress(): ZLinkKotlinSessionReplyCall
    suspend fun await()
}

interface ZLinkKotlinSessionClient {
    fun send(message: Any): ZLinkKotlinSessionSendCall
    fun reply(message: Any): ZLinkKotlinSessionReplyCall
}

interface ZLinkKotlinSessionActor {
    fun relay(message: systems.zlink.framework.messaging.ZLinkMessage):
        ZLinkKotlinSubmissionCall
    fun relay(
        dispatch: systems.zlink.framework.streams.ZLinkSessionDispatchContext,
        message: systems.zlink.framework.messaging.ZLinkMessage,
    ): ZLinkKotlinSubmissionCall
}

interface ZLinkKotlinBoundSession {
    fun send(message: Any): ZLinkKotlinMessageSendCall
}

interface ZLinkKotlinWorkerCall<T> {
    suspend fun await(): T
    suspend fun yield(): T
}
