package systems.zlink.e2e.kotlin.resiliencelifecycle

import java.util.logging.Handler
import java.util.logging.LogRecord
import java.util.logging.Logger

class EvidenceDispatchErrorHandler(
    private val state: ScenarioState,
) : Handler() {
    fun install() = Logger.getLogger(LOGGER_NAME).addHandler(this)

    override fun publish(record: LogRecord) {
        val fields = diagnosticsFields(record.message) ?: return
        if (fields["outcome"] != "ERROR") return
        state.record(
            "DispatchError",
            "${fields["reason"]}/${fields["action"]}/${fields["packet"]}",
        )
        if (state.observerThrows()) {
            throw IllegalStateException("diagnostics provider failure")
        }
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
