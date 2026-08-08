package systems.zlink.framework.kotlin

import java.nio.charset.StandardCharsets
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Test
import systems.zlink.contracts.messaging.Message
import systems.zlink.framework.ZLinkEncodedPayload
import systems.zlink.framework.ZLinkMessageSerializer
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer

final class KotlinMessageCodecBoundaryTest {
    @Test
    fun kotlinHandlersUseFrameworkMessageDecodeWithCustomSerializer() {
        val serializer = KotlinBoundarySerializer()
        val encoded = serializer.serialize(KotlinBoundaryPayload("custom"))

        val message = ZLinkMessage.fromEncoded(encoded, serializer)
        assertEquals(KotlinBoundaryPayload("custom"), message.decode<KotlinBoundaryPayload>())
    }

    @Test
    fun kotlinMessageOfKeepsTypedValueForFrameworkMessageDecode() {
        val message = messageOf(KotlinBoundaryPayload("typed"))

        assertEquals(KotlinBoundaryPayload("typed"), message.decode<KotlinBoundaryPayload>())
    }

    @Test
    fun defaultJsonSerializerDiscoversKotlinConstructorMetadata() {
        val serializer = ZLinkJsonMessageSerializer()
        val encoded = serializer.serialize(KotlinAcronymPayload("player-x"))

        assertEquals(
            KotlinAcronymPayload("player-x"),
            serializer.deserialize(encoded, KotlinAcronymPayload::class.java),
        )
    }
}

data class KotlinBoundaryPayload(val value: String)

data class KotlinAcronymPayload(val xActorId: String)

private class KotlinBoundarySerializer : ZLinkMessageSerializer {
    override fun <T : Any?> serialize(value: T): ZLinkEncodedPayload =
        when (value) {
            is Message -> ZLinkEncodedPayload.from(value.toByteArray())
            is KotlinBoundaryPayload -> ZLinkEncodedPayload.from(
                value.value.toByteArray(StandardCharsets.UTF_8),
            )
            else -> ZLinkEncodedPayload.from(value.toString().toByteArray(StandardCharsets.UTF_8))
        }

    override fun <T : Any?> deserialize(payload: ZLinkEncodedPayload, type: Class<T>): T {
        if (type == Message::class.java) {
            return type.cast(Message.from(payload.bytes()))
        }
        if (type == KotlinBoundaryPayload::class.java) {
            return type.cast(KotlinBoundaryPayload(String(payload.bytes(), StandardCharsets.UTF_8)))
        }
        throw IllegalArgumentException("unsupported message type: ${type.name}")
    }
}
