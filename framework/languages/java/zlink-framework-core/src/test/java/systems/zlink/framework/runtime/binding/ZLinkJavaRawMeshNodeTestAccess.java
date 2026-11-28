package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.InstanceSpotFollow;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.SpotRouteFence;

import java.util.List;
import java.util.concurrent.CompletionStage;
import java.util.function.BooleanSupplier;

/** Test-only access to MeshNode admission state. */
public final class ZLinkJavaRawMeshNodeTestAccess {
    private ZLinkJavaRawMeshNodeTestAccess() {}

    public static List<byte[]> instanceSpotFollowFrames(
            InstanceSpotFollow original, SpotRouteFence target, List<Message> parts) {
        return new ZLinkServiceM6BWireCodec()
                .encodeInstanceSpotFollow(
                        original,
                        target,
                        new byte[0],
                        ZLinkServiceM6AWireCodec.encodeFrameworkMultipartFrame(parts));
    }

    public static CompletionStage<Void> sendFrames(
            ZLinkInternalMeshNode sender, SpotRouteFence target, List<byte[]> frames) {
        var node = (ZLinkJavaRawMeshNode) sender;
        return node.port.send(node.router, target.targetNodeRid(), frames);
    }

    public static BooleanSupplier peerAdmissionSealed(ZLinkInternalMeshNode node) {
        return ((ZLinkJavaRawMeshNode) node).peerAdmissionSealed;
    }
}
