package systems.zlink.tutorial.client

import org.springframework.http.ResponseEntity
import org.springframework.web.bind.annotation.PostMapping
import org.springframework.web.bind.annotation.RequestBody
import org.springframework.web.bind.annotation.RestController
import systems.zlink.framework.channels.ZLinkFanoutClient
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.tutorial.shared.MaintenanceNotice

@RestController
class NoticeEndpoints(fanout: ZLinkFanoutClient) {

    private val fanout = fanout.kotlin()

    // --8<-- [start:fanout-call]
    @PostMapping("/notices")
    suspend fun publishNotice(@RequestBody notice: MaintenanceNotice): ResponseEntity<Void> {
        // Delivered to every subscriber. No recipient is named.
        fanout.publish("broadcast", notice).await()

        return ResponseEntity.accepted().build()
    }
    // --8<-- [end:fanout-call]
}
