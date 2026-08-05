package systems.zlink.framework.runtime.internal.handlers;

import java.lang.reflect.Method;
import java.util.concurrent.CompletionStage;

public interface ZLinkSuspendInvocationAdapter {
    boolean supports(Method method);

    CompletionStage<Object> invoke(Object handler, Method method, Object[] logicalArguments);
}
