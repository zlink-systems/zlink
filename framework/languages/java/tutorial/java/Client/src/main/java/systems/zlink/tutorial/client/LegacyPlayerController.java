package systems.zlink.tutorial.client;

import java.net.URI;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.RestController;

@RestController
final class LegacyPlayerController {

    @GetMapping("/player/{playerId}")
    ResponseEntity<Void> redirect(@PathVariable String playerId) {
        return ResponseEntity.status(301)
            .location(URI.create("/players/" + playerId))
            .build();
    }
}
