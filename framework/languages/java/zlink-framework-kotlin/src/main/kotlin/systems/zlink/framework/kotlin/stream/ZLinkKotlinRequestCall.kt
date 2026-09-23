package systems.zlink.framework.kotlin.stream

import java.time.Duration
import kotlin.reflect.KClass
import kotlinx.coroutines.future.await
import systems.zlink.stream.connector.ZLinkTypedStreamRequestCall

/** The typed STREAM request builder; the framework request contract owns the root package name. */
class ZLinkKotlinRequestCall<TReply : Any>(
    private val inner: ZLinkTypedStreamRequestCall,
    private val replyType: KClass<TReply>,
) {
    fun packetName(name: String): ZLinkKotlinRequestCall<TReply> =
        ZLinkKotlinRequestCall(inner.packetName(name), replyType)

    fun metadata(key: String, value: String): ZLinkKotlinRequestCall<TReply> =
        ZLinkKotlinRequestCall(inner.metadata(key, value), replyType)

    fun timeout(timeout: Duration): ZLinkKotlinRequestCall<TReply> =
        ZLinkKotlinRequestCall(inner.timeout(timeout), replyType)

    fun compress(): ZLinkKotlinRequestCall<TReply> =
        ZLinkKotlinRequestCall(inner.compress(), replyType)

    suspend fun await(): TReply = inner.submit(replyType.java).await()
}
