package systems.zlink.tutorial.server

import org.springframework.web.bind.annotation.PathVariable
import org.springframework.web.bind.annotation.PostMapping
import org.springframework.web.bind.annotation.RequestParam
import org.springframework.web.bind.annotation.RestController
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions

/** The only HTTP this node serves. Everything else it does arrives over the mesh. */
@RestController
class AdminEndpoints(private val mesh: ZLinkRouteMeshRuntimeOptions) {

    // --8<-- [start:weight-runtime]
    // Weight is the one value this node can change while running. 0 keeps the
    // socket open and finishes in-flight work, but other nodes stop choosing this
    // one for new calls. 100 is the normal value.
    //
    // ZLinkRouteMeshRuntimeOptions is the Java surface, injected as a bean. There
    // is no Kotlin projection of it, and weight() is a plain method pair rather
    // than a getter/setter, so it is read and written by call and not by property.
    @PostMapping("/admin/channels/{channel}/weight")
    fun setWeight(@PathVariable channel: String, @RequestParam value: Int): WeightChanged {
        mesh.channel(channel).weight(value)
        return WeightChanged(channel, value)
    }

    data class WeightChanged(val channel: String, val weight: Int)
    // --8<-- [end:weight-runtime]
}
