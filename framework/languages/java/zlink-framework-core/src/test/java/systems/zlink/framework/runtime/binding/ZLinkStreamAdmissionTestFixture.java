package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.Context;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.runtime.framework.FrameworkStreamOperations;

import java.util.function.UnaryOperator;

public final class ZLinkStreamAdmissionTestFixture {
    private ZLinkStreamAdmissionTestFixture() {}

    public static ZLinkBackendStreamSocket create(
            Context context, UnaryOperator<FrameworkStreamOperations.Access> wrapAdmission) {
        return new ZLinkJavaStreamSocket(
                context.createStreamSocket(),
                new ZLinkStateLane(Runnable::run),
                wrapAdmission.apply(FrameworkStreamOperations::send));
    }
}
