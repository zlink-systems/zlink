package systems.zlink.e2e.kotlin.resiliencelifecycle

import com.fasterxml.jackson.annotation.JsonCreator
import com.fasterxml.jackson.annotation.JsonProperty

object Contracts {
    const val CHANNEL: String = "resilience.lifecycle.api"
    const val HANDLER_GROUP: String = "resilience-lifecycle"

    data class WorkReq @JsonCreator constructor(
        @JsonProperty("value") val value: String,
    ) {
        fun value(): String = value
    }

    data class WorkMsg @JsonCreator constructor(
        @JsonProperty("value") val value: String,
    ) {
        fun value(): String = value
    }

    data class UnhandledReq @JsonCreator constructor(
        @JsonProperty("value") val value: String,
    ) {
        fun value(): String = value
    }

    data class WorkRes @JsonCreator constructor(
        @JsonProperty("value") val value: String,
        @JsonProperty("providerRid") val providerRid: String,
    ) {
        fun value(): String = value

        fun providerRid(): String = providerRid
    }

    data class EvidenceEntry @JsonCreator constructor(
        @JsonProperty("marker") val marker: String,
        @JsonProperty("providerRid") val providerRid: String,
        @JsonProperty("value") val value: String,
    ) {
        fun marker(): String = marker

        fun providerRid(): String = providerRid

        fun value(): String = value
    }

    data class EvidenceSnapshot @JsonCreator constructor(
        @JsonProperty("providerRid") val providerRid: String,
        @JsonProperty("weight") val weight: Int,
        @JsonProperty("entries") val entries: List<EvidenceEntry>,
    ) {
        fun providerRid(): String = providerRid

        fun weight(): Int = weight

        fun entries(): List<EvidenceEntry> = entries
    }

    data class TopologyWaitReq @JsonCreator constructor(
        @JsonProperty("expectedRouters") val expectedRouters: Int,
        @JsonProperty("routingId") val routingId: String?,
        @JsonProperty("endpoint") val endpoint: String?,
    )

    data class TopologyWaitRes @JsonCreator constructor(
        @JsonProperty("matchedRouters") val matchedRouters: Int,
    )

    data class TopologyReadRes @JsonCreator constructor(
        @JsonProperty("status") val status: String,
        @JsonProperty("matchedRouters") val matchedRouters: Int,
        @JsonProperty("error") val error: String?,
    )
}
