package systems.zlink.tutorial.client;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.ObjectMapper;

import jakarta.servlet.http.HttpServletRequest;

import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RestController;
import org.springframework.web.servlet.mvc.method.annotation.StreamingResponseBody;

import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.tutorial.shared.Contracts;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

@RestController
final class HttpOperationsController {

    private static final MediaType NDJSON = MediaType.valueOf("application/x-ndjson");

    private final ZLinkRouteClient route;
    private final ObjectMapper mapper;

    HttpOperationsController(ZLinkRouteClient route, ObjectMapper mapper) {
        this.route = route;
        this.mapper = mapper;
    }

    @GetMapping("/rooms/{roomId}/export")
    CompletionStage<ResponseEntity<StreamingResponseBody>> exportRoom(@PathVariable String roomId) {
        return route.requestToSpot(roomId, new Contracts.GetRoomState())
                .submit(Contracts.RoomState.class)
                .thenApply(state -> roomExport(roomId, state));
    }

    private ResponseEntity<StreamingResponseBody> roomExport(
            String roomId, Contracts.RoomState state) {
        return ResponseEntity.ok()
                .contentType(NDJSON)
                .body(output -> writeRoom(output, roomId, state));
    }

    private void writeRoom(OutputStream output, String roomId, Contracts.RoomState state)
            throws IOException {
        writeLine(output, new RoomLine(roomId, state.title(), null));
        for (String message : state.chat()) {
            writeLine(output, new RoomLine(null, null, message));
        }
    }

    @PostMapping("/rooms/{roomId}/import")
    CompletionStage<Imported> importRoom(@PathVariable String roomId, HttpServletRequest request)
            throws IOException {
        return readNext(
                new BufferedReader(
                        new InputStreamReader(request.getInputStream(), StandardCharsets.UTF_8)),
                roomId,
                0);
    }

    private CompletionStage<Imported> readNext(BufferedReader reader, String roomId, int count) {
        return CompletableFuture.supplyAsync(() -> readLine(reader))
                .thenCompose(
                        line -> {
                            if (line == null) {
                                close(reader);
                                return CompletableFuture.completedFuture(new Imported(count));
                            }
                            Contracts.PostChat message = parseChat(line);
                            return route.sendToSpot(roomId, message)
                                    .submit()
                                    .thenCompose(ignored -> readNext(reader, roomId, count + 1));
                        });
    }

    private static String readLine(BufferedReader reader) {
        try {
            return reader.readLine();
        } catch (IOException error) {
            throw new CompletionException(error);
        }
    }

    private Contracts.PostChat parseChat(String line) {
        try {
            return mapper.readValue(line, Contracts.PostChat.class);
        } catch (JsonProcessingException error) {
            throw new CompletionException(error);
        }
    }

    private void writeLine(OutputStream output, RoomLine value) throws IOException {
        output.write(mapper.writeValueAsBytes(value));
        output.write('\n');
        output.flush();
    }

    private static void close(BufferedReader reader) {
        try {
            reader.close();
        } catch (IOException error) {
            throw new CompletionException(error);
        }
    }

    private record RoomLine(String roomId, String title, String message) {}

    private record Imported(int imported) {}
}
