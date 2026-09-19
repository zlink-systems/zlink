package systems.zlink.tutorial.client

import com.fasterxml.jackson.databind.ObjectMapper
import java.io.OutputStream
import jakarta.servlet.http.HttpServletRequest
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.future.await
import kotlinx.coroutines.withContext
import org.springframework.http.MediaType
import org.springframework.http.ResponseEntity
import org.springframework.web.bind.annotation.GetMapping
import org.springframework.web.bind.annotation.PathVariable
import org.springframework.web.bind.annotation.PostMapping
import org.springframework.web.bind.annotation.RestController
import org.springframework.web.servlet.mvc.method.annotation.StreamingResponseBody
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.tutorial.shared.GetRoomState
import systems.zlink.tutorial.shared.PostChat
import systems.zlink.tutorial.shared.RoomState

@RestController
class HttpOperationsController(
    route: ZLinkRouteClient,
    private val mapper: ObjectMapper,
) {

    private val route = route

    @GetMapping("/rooms/{roomId}/export")
    suspend fun exportRoom(@PathVariable roomId: String): ResponseEntity<StreamingResponseBody> {
        val state = route.requestToSpot(roomId, GetRoomState())
            .submit(RoomState::class.java)
            .await()
        val body = StreamingResponseBody { output ->
            writeLine(output, RoomLine(roomId, state.title, null))
            state.chat.forEach { message ->
                writeLine(output, RoomLine(null, null, message))
            }
        }
        return ResponseEntity.ok()
            .contentType(MediaType.valueOf("application/x-ndjson"))
            .body(body)
    }

    @PostMapping("/rooms/{roomId}/import")
    suspend fun importRoom(
        @PathVariable roomId: String,
        request: HttpServletRequest,
    ): Imported {
        val reader = request.inputStream.bufferedReader()
        var count = 0
        try {
            while (true) {
                val line = withContext(Dispatchers.IO) { reader.readLine() } ?: break
                val message = mapper.readValue(line, PostChat::class.java)
                route.sendToSpot(roomId, message).submit().await()
                count += 1
            }
        } finally {
            withContext(Dispatchers.IO) { reader.close() }
        }
        return Imported(count)
    }

    private fun writeLine(output: OutputStream, value: RoomLine) {
        output.write(mapper.writeValueAsBytes(value))
        output.write('\n'.code)
        output.flush()
    }

    private data class RoomLine(val roomId: String?, val title: String?, val message: String?)
    data class Imported(val imported: Int)
}
