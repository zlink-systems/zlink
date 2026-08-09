package Handlers


import systems.zlink.e2e.kotlin.registrymessaging.workflow.Handlers
import systems.zlink.e2e.kotlin.registrymessaging.workflow.Infrastructure
import java.util.logging.Handler
import java.util.logging.LogRecord
import java.util.logging.Logger
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.e2e.kotlin.registrymessaging.shared.WorkflowRes
import systems.zlink.e2e.kotlin.registrymessaging.shared.WorkflowReq
import systems.zlink.e2e.kotlin.registrymessaging.shared.Contracts
import systems.zlink.e2e.kotlin.registrymessaging.workflow.Infrastructure.EvidenceStore
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler

@ZLinkHandlerGroup(Contracts.CHANNEL_HANDLER_GROUP)
class WorkflowRequestHandler(
    private val evidence: EvidenceStore,
) : ZLinkSuspendingRequestHandler<WorkflowReq, WorkflowRes> {
    override suspend fun handle(request: WorkflowReq, context: ZLinkMessageContext): WorkflowRes {
        evidence.add("workflow-request|rid=${evidence.rid}|value=${request.value}|packet=${context.packetName()}")
        return WorkflowRes("workflow:${request.value}", evidence.rid)
    }
}

class EvidenceDispatchErrorHandler(
    private val evidence: EvidenceStore,
) : Handler() {
    fun install() = Logger.getLogger(LOGGER_NAME).addHandler(this)

    override fun publish(record: LogRecord) {
        val fields = diagnosticsFields(record.message) ?: return
        if (fields["outcome"] != "ERROR") return
        evidence.add(
            "dispatch-error" +
                "|surface=${fields["surface"]}" +
                "|kind=${fields["kind"]}" +
                "|reason=${fields["reason"]}" +
                "|action=${fields["action"]}" +
                "|packet=${fields["packet"] ?: "<null>"}" +
                "|channel=${fields["channel"] ?: "<null>"}",
        )
    }

    override fun flush() = Unit
    override fun close() = Logger.getLogger(LOGGER_NAME).removeHandler(this)

    private fun diagnosticsFields(message: String?): Map<String, String>? {
        if (message?.startsWith("message flow ") != true) return null
        return message.split(' ').mapNotNull { token ->
            val separator = token.indexOf('=')
            if (separator > 0) token.substring(0, separator) to token.substring(separator + 1) else null
        }.toMap()
    }

    companion object {
        private const val LOGGER_NAME = "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer"
    }
}
