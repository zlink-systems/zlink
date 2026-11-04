package systems.zlink.framework.runtime.mesh;

public final class MeshNodeRegistrationTestAccess {
    private MeshNodeRegistrationTestAccess() {}

    public static void clearEntrySpots(MeshNodeRegistration registration) {
        registration.entrySpots.clear();
    }
}
