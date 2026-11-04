package systems.zlink.framework.runtime.binding;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;

import java.util.function.BooleanSupplier;

/** Test-only access to MeshNode admission state. */
public final class ZLinkJavaRawMeshNodeTestAccess {
    private ZLinkJavaRawMeshNodeTestAccess() {}

    public static BooleanSupplier peerAdmissionSealed(ZLinkInternalMeshNode node) {
        return ((ZLinkJavaRawMeshNode) node).peerAdmissionSealed;
    }
}
