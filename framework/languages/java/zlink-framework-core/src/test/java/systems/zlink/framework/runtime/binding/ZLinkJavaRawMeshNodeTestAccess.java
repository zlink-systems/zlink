package systems.zlink.framework.runtime.binding;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;

import java.util.function.BooleanSupplier;

/** Test-only access to MeshNode state. */
public final class ZLinkJavaRawMeshNodeTestAccess {
    private ZLinkJavaRawMeshNodeTestAccess() {}

    public static ZLinkServiceOperationRegistry operationRegistry(ZLinkInternalMeshNode node) {
        return ((ZLinkJavaRawMeshNode) node).operationRegistry();
    }

    public static BooleanSupplier peerAdmissionSealed(ZLinkInternalMeshNode node) {
        return ((ZLinkJavaRawMeshNode) node).peerAdmissionSealed;
    }
}
