package systems.zlink.framework.runtime.internal.drain;

import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/** Coordinates application admission and its drain barrier per RouteMesh. */
public final class ZLinkMeshDrainCoordinator {
    private final Map<String, MeshState> meshes = new LinkedHashMap<>();

    public ZLinkMeshDrainCoordinator(Collection<String> meshNames) {
        Objects.requireNonNull(meshNames, "meshNames");
        for (String meshName : meshNames) {
            meshes.put(Objects.requireNonNull(meshName, "meshName"), new MeshState());
        }
    }

    public Claim tryClaim(String meshName) {
        MeshState state = requireMesh(meshName);
        synchronized (state) {
            if (state.sealed) {
                return null;
            }
            state.accepted++;
            return new Claim(state);
        }
    }

    public CompletionStage<Void> sealAndAwaitZero(String meshName) {
        seal(meshName);
        return awaitZero(meshName);
    }

    public void seal(String meshName) {
        MeshState state = requireMesh(meshName);
        synchronized (state) {
            state.sealed = true;
            if (state.accepted == 0) {
                state.zero.complete(null);
            }
        }
    }

    public CompletionStage<Void> awaitZero(String meshName) {
        MeshState state = requireMesh(meshName);
        synchronized (state) {
            return state.zero;
        }
    }

    public void sealAll() {
        meshes.keySet().forEach(this::seal);
    }

    public CompletionStage<Void> awaitAllZero() {
        CompletableFuture<?>[] barriers = meshes.keySet().stream()
            .map(this::awaitZero)
            .map(CompletionStage::toCompletableFuture)
            .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(barriers);
    }

    public boolean isSealed(String meshName) {
        MeshState state = requireMesh(meshName);
        synchronized (state) {
            return state.sealed;
        }
    }

    private MeshState requireMesh(String meshName) {
        MeshState state = meshes.get(meshName);
        if (state == null) {
            throw new IllegalArgumentException("unknown RouteMesh: " + meshName);
        }
        return state;
    }

    private static final class MeshState {
        private final CompletableFuture<Void> zero = new CompletableFuture<>();
        private boolean sealed;
        private int accepted;
    }

    public static final class Claim implements AutoCloseable {
        private MeshState state;

        private Claim(MeshState state) {
            this.state = state;
        }

        @Override
        public void close() {
            MeshState current = state;
            if (current == null) {
                return;
            }
            state = null;
            synchronized (current) {
                current.accepted--;
                if (current.accepted == 0 && current.sealed) {
                    current.zero.complete(null);
                }
            }
        }
    }
}
