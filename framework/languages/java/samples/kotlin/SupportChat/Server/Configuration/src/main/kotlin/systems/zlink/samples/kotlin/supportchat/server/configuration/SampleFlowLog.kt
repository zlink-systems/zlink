package systems.zlink.samples.kotlin.supportchat.server.configuration

object SampleFlowLog {
    fun path(topology: SampleTopology, role: String): String =
        "${topology.logDirectory()}/flow-$role.log"
}
