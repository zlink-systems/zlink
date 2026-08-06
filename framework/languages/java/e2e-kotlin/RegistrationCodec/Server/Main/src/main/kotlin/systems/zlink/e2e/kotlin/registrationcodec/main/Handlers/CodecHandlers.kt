package systems.zlink.e2e.kotlin.registrationcodec.main.handlers

import com.google.protobuf.StringValue
import systems.zlink.e2e.kotlin.registrationcodec.Contracts
import systems.zlink.e2e.kotlin.registrationcodec.JsonEchoMsg
import systems.zlink.e2e.kotlin.registrationcodec.JsonEchoReq
import systems.zlink.e2e.kotlin.registrationcodec.JsonEchoRes
import systems.zlink.e2e.kotlin.registrationcodec.JsonGoldenReq
import systems.zlink.e2e.kotlin.registrationcodec.JsonGoldenRes
import systems.zlink.e2e.kotlin.registrationcodec.PackedEchoMsg
import systems.zlink.e2e.kotlin.registrationcodec.PackedEchoRes
import systems.zlink.e2e.kotlin.registrationcodec.PackedEchoReq
import systems.zlink.e2e.kotlin.registrationcodec.main.infrastructure.ScenarioState
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSendHandler

@ZLinkHandlerGroup(Contracts.MANUAL_GROUP)
class JsonRequestHandler(
    private val state: ScenarioState,
) : ZLinkSuspendingRequestHandler<JsonEchoReq, JsonEchoRes> {
    override suspend fun handle(request: JsonEchoReq, context: ZLinkMessageContext): JsonEchoRes {
        state.record("Request", "JsonEchoReq", request.value)
        return JsonEchoRes("echo:${request.value}", "json")
    }
}

@ZLinkHandlerGroup(Contracts.MANUAL_GROUP)
class JsonSendHandler(
    private val state: ScenarioState,
) : ZLinkSuspendingSendHandler<JsonEchoMsg> {
    override suspend fun handle(message: JsonEchoMsg, context: ZLinkMessageContext) {
        state.record("Send", "JsonEchoMsg", message.value)
    }
}

@ZLinkHandlerGroup(Contracts.MANUAL_GROUP)
class JsonGoldenRequestHandler(
    private val state: ScenarioState,
) : ZLinkSuspendingRequestHandler<JsonGoldenReq, JsonGoldenRes> {
    override suspend fun handle(request: JsonGoldenReq, context: ZLinkMessageContext): JsonGoldenRes {
        state.record(
            "Request",
            "JsonGoldenReq",
            "${request.displayName}|${request.status}|${request.balance}|${request.score}|${request.payload.size}",
        )
        return JsonGoldenRes(
            request.displayName,
            request.status,
            request.balance,
            request.payload,
            request.score,
            request.ratio,
            request.optionalNote,
            context.contentType().orElse("<null>"),
        )
    }
}

@ZLinkHandlerGroup(Contracts.MANUAL_GROUP)
class ProtobufRequestHandler(
    private val state: ScenarioState,
) : ZLinkSuspendingRequestHandler<StringValue, StringValue> {
    override suspend fun handle(request: StringValue, context: ZLinkMessageContext): StringValue {
        state.record("Request", "ProtobufEcho", request.value)
        return StringValue.of("echo:${request.value}")
    }
}

@ZLinkHandlerGroup(Contracts.MANUAL_GROUP)
class ProtobufSendHandler(
    private val state: ScenarioState,
) : ZLinkSuspendingSendHandler<StringValue> {
    override suspend fun handle(message: StringValue, context: ZLinkMessageContext) {
        state.record("Send", "ProtobufEcho", message.value)
    }
}

@ZLinkHandlerGroup(Contracts.MANUAL_GROUP)
class MsgpackRequestHandler(
    private val state: ScenarioState,
) : ZLinkSuspendingRequestHandler<PackedEchoReq, PackedEchoRes> {
    override suspend fun handle(request: PackedEchoReq, context: ZLinkMessageContext): PackedEchoRes {
        state.record("Request", "PackedEchoReq", request.value)
        return PackedEchoRes("echo:${request.value}")
    }
}

@ZLinkHandlerGroup(Contracts.MANUAL_GROUP)
class MsgpackSendHandler(
    private val state: ScenarioState,
) : ZLinkSuspendingSendHandler<PackedEchoMsg> {
    override suspend fun handle(message: PackedEchoMsg, context: ZLinkMessageContext) {
        state.record("Send", "PackedEchoMsg", message.value)
    }
}
