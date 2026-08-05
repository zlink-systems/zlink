package systems.zlink.e2e.kotlin.registrationcodec.codecrequester

import com.google.protobuf.StringValue
import java.time.Duration
import java.util.concurrent.CompletionStage
import systems.zlink.e2e.kotlin.registrationcodec.Contracts
import systems.zlink.e2e.kotlin.registrationcodec.JsonEchoReq
import systems.zlink.e2e.kotlin.registrationcodec.JsonEchoRes
import systems.zlink.framework.channels.ZLinkClient

class CodecRequesterProbe(
    private val client: ZLinkClient,
) {
    fun requestJson(): CompletionStage<JsonEchoRes> =
        client.requestToChannel(Contracts.CHANNEL, JsonEchoReq("json-from-requester"))
            .timeout(Duration.ofSeconds(5))
            .submit(JsonEchoRes::class.java)

    fun requestProtobuf(): CompletionStage<CodecMismatchProbeRes> =
        client.requestToChannel(Contracts.CHANNEL, StringValue.of("protobuf-mismatch"))
                .timeout(Duration.ofSeconds(2))
                .submit(StringValue::class.java)
            .handle { reply, error ->
                if (error == null) CodecMismatchProbeRes(false, null, reply.value)
                else CodecMismatchProbeRes(true, error.javaClass.simpleName, null)
            }
}

data class CodecMismatchProbeRes(
    val error: Boolean,
    val errorType: String?,
    val value: String?,
)
