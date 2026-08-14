package systems.zlink.framework.runtime.binding;

import java.time.Duration;

interface ZLinkJavaAdmissionBacked {
    default Duration admissionTimeout() {
        return Duration.ofSeconds(1);
    }
}
