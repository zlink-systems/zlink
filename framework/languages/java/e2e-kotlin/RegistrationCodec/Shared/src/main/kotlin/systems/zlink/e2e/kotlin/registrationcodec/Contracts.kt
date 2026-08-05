package systems.zlink.e2e.kotlin.registrationcodec

import systems.zlink.framework.handlers.ZLinkPacket

object Contracts {
    const val CHANNEL = "registration.codec.kotlin.api"
    const val AUTO_GROUP = "registration-codec-kotlin-auto"
    const val ATTR_GROUP = "registration-codec-kotlin-attr"
    const val MANUAL_GROUP = "registration-codec-kotlin-manual"
}

@ZLinkPacket("EchoAutoReq")
class EchoAutoReq() {
    var value: String = ""
    constructor(value: String) : this() { this.value = value }
}

@ZLinkPacket("EchoAutoMsg")
class EchoAutoMsg() {
    var value: String = ""
    constructor(value: String) : this() { this.value = value }
}

class EchoAutoRes() {
    var value: String = ""
    var handler: String = ""
    constructor(value: String, handler: String) : this() {
        this.value = value
        this.handler = handler
    }
}

class EchoAttrReq() {
    var value: String = ""
    constructor(value: String) : this() { this.value = value }
}

class EchoAttrMsg() {
    var value: String = ""
    constructor(value: String) : this() { this.value = value }
}

class EchoAttrRes() {
    var value: String = ""
    var handler: String = ""
    constructor(value: String, handler: String) : this() {
        this.value = value
        this.handler = handler
    }
}

class EchoManualReq() {
    var value: String = ""
    constructor(value: String) : this() { this.value = value }
}

class EchoManualMsg() {
    var value: String = ""
    constructor(value: String) : this() { this.value = value }
}

class EchoManualRes() {
    var value: String = ""
    var handler: String = ""
    constructor(value: String, handler: String) : this() {
        this.value = value
        this.handler = handler
    }
}

class DiLifecycleReq() {
    var value: String = ""
    constructor(value: String) : this() { this.value = value }
}

class DiLifecycleRes() {
    var value: String = ""
    var scopedId: Int = 0
    var singletonId: Int = 0
    var disposedCount: Int = 0

    constructor(value: String, scopedId: Int, singletonId: Int, disposedCount: Int) : this() {
        this.value = value
        this.scopedId = scopedId
        this.singletonId = singletonId
        this.disposedCount = disposedCount
    }
}

class JsonEchoReq() {
    var value: String = ""
    constructor(value: String) : this() { this.value = value }
}

class JsonEchoMsg() {
    var value: String = ""
    constructor(value: String) : this() { this.value = value }
}

class JsonEchoRes() {
    var value: String = ""
    var handler: String = ""
    constructor(value: String, handler: String) : this() {
        this.value = value
        this.handler = handler
    }
}

class PackedEchoReq() {
    var value: String = ""
    constructor(value: String) : this() { this.value = value }
}

class PackedEchoRes() {
    var value: String = ""
    constructor(value: String) : this() { this.value = value }
}

class PackedEchoMsg() {
    var value: String = ""
    constructor(value: String) : this() { this.value = value }
}

class EvidenceEntry() {
    var marker: String = ""
    var packetName: String = ""
    var value: String = ""

    constructor(marker: String, packetName: String, value: String) : this() {
        this.marker = marker
        this.packetName = packetName
        this.value = value
    }
}

class EvidenceSnapshot() {
    var entries: List<EvidenceEntry> = emptyList()
    constructor(entries: List<EvidenceEntry>) : this() { this.entries = entries }
}

class EvidenceWaitReq() {
    var marker: String = ""
    var packetName: String = ""
    var value: String = ""
    var timeoutMillis: Long = 3000

    constructor(marker: String, packetName: String, value: String, timeoutMillis: Long = 3000) : this() {
        this.marker = marker
        this.packetName = packetName
        this.value = value
        this.timeoutMillis = timeoutMillis
    }
}

class EvidenceWaitRes() {
    var entries: List<EvidenceEntry> = emptyList()
    constructor(entries: List<EvidenceEntry>) : this() { this.entries = entries }
}

class CodecRoundtripRes() {
    var jsonValue: String = ""
    var protobufValue: String = ""
    var messagePackValue: String = ""

    constructor(jsonValue: String, protobufValue: String, messagePackValue: String) : this() {
        this.jsonValue = jsonValue
        this.protobufValue = protobufValue
        this.messagePackValue = messagePackValue
    }
}

fun isPackedType(type: Class<*>): Boolean =
    type == PackedEchoReq::class.java ||
        type == PackedEchoRes::class.java ||
        type == PackedEchoMsg::class.java
