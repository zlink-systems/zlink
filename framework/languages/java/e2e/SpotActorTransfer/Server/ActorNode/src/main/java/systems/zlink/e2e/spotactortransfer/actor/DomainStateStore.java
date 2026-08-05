package systems.zlink.e2e.spotactortransfer.actor;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

public final class DomainStateStore {
    private final Path directory;

    public DomainStateStore(String directory) {
        this.directory = Path.of(directory);
        try {
            Files.createDirectories(this.directory);
        } catch (IOException error) {
            throw new IllegalStateException("failed to prepare domain state directory", error);
        }
    }

    public void save(String actorId, int stateVersion) {
        try {
            Files.writeString(path(actorId), Integer.toString(stateVersion));
        } catch (IOException error) {
            throw new IllegalStateException("failed to save actor domain state: " + actorId, error);
        }
    }

    public int load(String actorId) {
        try {
            Path path = path(actorId);
            return Files.exists(path) ? Integer.parseInt(Files.readString(path).trim()) : 0;
        } catch (IOException error) {
            throw new IllegalStateException("failed to load actor domain state: " + actorId, error);
        }
    }

    private Path path(String actorId) {
        return directory.resolve(actorId.replaceAll("[^A-Za-z0-9_.-]", "_") + ".state");
    }
}
