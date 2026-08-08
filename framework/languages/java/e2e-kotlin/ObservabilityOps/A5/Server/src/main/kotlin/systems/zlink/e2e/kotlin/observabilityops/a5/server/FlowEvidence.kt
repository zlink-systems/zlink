package systems.zlink.e2e.kotlin.observabilityops.a5.server

import java.util.concurrent.CopyOnWriteArrayList
import java.util.logging.Handler
import java.util.logging.LogRecord
import java.util.logging.Logger

class FlowEvidence : Handler() {
    private val events = CopyOnWriteArrayList<FlowEvent>()

    fun install() = Logger.getLogger(LOGGER_NAME).addHandler(this)

    override fun publish(record: LogRecord) {
        FlowEvent.parse(record.message)?.let(events::add)
    }

    override fun flush() = Unit
    override fun close() = Logger.getLogger(LOGGER_NAME).removeHandler(this)

    fun snapshot(): List<FlowEvent> = events.toList()

    data class FlowEvent(
        val outcome: String?,
        val surface: String?,
        val messageKind: String?,
        val packetName: String?,
        val channelName: String?,
        val errorReason: String?,
        val errorType: String?,
    ) {
        companion object {
            fun parse(message: String?): FlowEvent? {
                if (message?.startsWith("message flow ") != true) return null
                val fields = message.split(' ')
                    .mapNotNull { token ->
                        val separator = token.indexOf('=')
                        if (separator > 0) token.substring(0, separator) to token.substring(separator + 1)
                        else null
                    }
                    .toMap()
                return FlowEvent(
                    fields["outcome"], fields["surface"], fields["kind"],
                    fields["packet"], fields["channel"], fields["reason"], fields["errorType"],
                )
            }
        }
    }

    companion object {
        private const val LOGGER_NAME =
            "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer"
    }
}
