package systems.zlink.e2e.kotlin.pubsub.shared

object Contracts {
    const val EVENT_CHANNEL = "pubsub.kotlin.events"
    const val HANDLER_GROUP = "pubsub-kotlin"
    const val EVENT_PACKET = "EventMsg"
}

class EventMsg() {
    var scenario: String = ""
    var sequence: Int = 0
    var value: String = ""

    constructor(
        scenario: String,
        sequence: Int,
        value: String,
    ) : this() {
        this.scenario = scenario
        this.sequence = sequence
        this.value = value
    }
}

class MissingEventMsg(
    var scenario: String = "",
    var sequence: Int = 0,
    var value: String = "",
)

class EvidenceEntry() {
    var marker: String = ""
    var subscriberRid: String = ""
    var topic: String = ""
    var scenario: String = ""
    var sequence: Int = 0
    var value: String = ""

    constructor(
        marker: String,
        subscriberRid: String,
        topic: String,
        scenario: String,
        sequence: Int,
        value: String,
    ) : this() {
        this.marker = marker
        this.subscriberRid = subscriberRid
        this.topic = topic
        this.scenario = scenario
        this.sequence = sequence
        this.value = value
    }
}

class EvidenceSnapshot() {
    var subscriberRid: String = ""
    var entries: List<EvidenceEntry> = emptyList()

    constructor(
        subscriberRid: String,
        entries: List<EvidenceEntry>,
    ) : this() {
        this.subscriberRid = subscriberRid
        this.entries = entries
    }
}
