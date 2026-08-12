package systems.zlink.framework.runtime.configuration;
import java.util.concurrent.atomic.AtomicReference;

import systems.zlink.framework.configuration.ZLinkDiagnosticsOptions;
import systems.zlink.framework.configuration.ZLinkDispatchOptions;
import systems.zlink.framework.configuration.ZLinkLogLevel;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction;
import systems.zlink.framework.configuration.ZLinkUnhandledDispatchOptions;
import systems.zlink.framework.errors.ZLinkConfigurationException;

public final class ZLinkDispatchOptionsRegistration implements ZLinkDispatchOptions {
    private final UnhandledDispatchOptions unhandled = new UnhandledDispatchOptions();
    private final DiagnosticsOptions diagnostics = new DiagnosticsOptions();

    @Override
    public UnhandledDispatchOptions unhandled() {
        return unhandled;
    }

    @Override
    public DiagnosticsOptions diagnostics() {
        return diagnostics;
    }

    @Override
    public ZLinkDispatchOptions messageFlow(ZLinkMessageFlowLogMode mode) {
        diagnostics.setMessageFlow(mode);
        return this;
    }

    @Override
    public ZLinkDispatchOptions traceSampleRate(double rate) {
        diagnostics.setSampleRate(rate);
        return this;
    }

    @Override
    public ZLinkDispatchOptions includeMessageSizes(boolean include) {
        diagnostics.setIncludeMessageSizes(include);
        return this;
    }

    void validate() {
        if (unhandled.send() == ZLinkUnhandledDispatchAction.REPLY_ERROR) {
            throw new ZLinkConfigurationException(
                "unhandled send dispatch cannot use REPLY_ERROR because send has no reply path");
        }
        if (unhandled.publish() == ZLinkUnhandledDispatchAction.REPLY_ERROR) {
            throw new ZLinkConfigurationException(
                "unhandled publish dispatch cannot use REPLY_ERROR because publish has no reply path");
        }
        double sampleRate = diagnostics.sampleRate();
        if (Double.isNaN(sampleRate) || sampleRate < 0.0d || sampleRate > 1.0d) {
            throw new ZLinkConfigurationException(
                "diagnostics sample rate must be between 0.0 and 1.0");
        }
    }

    private static <T> T requireNonNull(T value, String name) {
        if (value == null) {
            throw new ZLinkConfigurationException(name + " is required");
        }
        return value;
    }

    public static final class UnhandledDispatchOptions implements ZLinkUnhandledDispatchOptions {
        private ZLinkUnhandledDispatchAction request = ZLinkUnhandledDispatchAction.REPLY_ERROR;
        private ZLinkUnhandledDispatchAction send = ZLinkUnhandledDispatchAction.LOG_AND_DROP;
        private ZLinkUnhandledDispatchAction publish = ZLinkUnhandledDispatchAction.LOG_AND_DROP;
        private ZLinkLogLevel sendLogLevel = ZLinkLogLevel.WARN;
        private ZLinkLogLevel publishLogLevel = ZLinkLogLevel.DEBUG;

        public ZLinkUnhandledDispatchAction request() {
            return request;
        }

        public ZLinkUnhandledDispatchAction send() {
            return send;
        }

        public ZLinkUnhandledDispatchAction publish() {
            return publish;
        }

        public ZLinkLogLevel sendLogLevel() {
            return sendLogLevel;
        }

        public ZLinkLogLevel publishLogLevel() {
            return publishLogLevel;
        }

        @Override
        public void setRequest(ZLinkUnhandledDispatchAction action) {
            request = requireNonNull(action, "request");
        }

        @Override
        public void setSend(ZLinkUnhandledDispatchAction action) {
            send = requireNonNull(action, "send");
        }

        @Override
        public void setPublish(ZLinkUnhandledDispatchAction action) {
            publish = requireNonNull(action, "publish");
        }

        @Override
        public void setSendLogLevel(ZLinkLogLevel level) {
            sendLogLevel = requireNonNull(level, "sendLogLevel");
        }

        @Override
        public void setPublishLogLevel(ZLinkLogLevel level) {
            publishLogLevel = requireNonNull(level, "publishLogLevel");
        }
    }

    public static final class DiagnosticsOptions implements ZLinkDiagnosticsOptions {
        private ZLinkMessageFlowLogMode messageFlow = ZLinkMessageFlowLogMode.ERRORS;
        private double sampleRate = 1.0d;
        private boolean includeMessageSizes = true;
        // Shared, runtime-mutable mode cell installed by the host at start; shared
        // across surfaces so setMessageFlowMode flips it live. Null before install.
        private volatile AtomicReference<ZLinkMessageFlowLogMode> liveMode;

        public ZLinkMessageFlowLogMode messageFlow() {
            return messageFlow;
        }

        public double sampleRate() {
            return sampleRate;
        }

        public boolean includeMessageSizes() {
            return includeMessageSizes;
        }

        // Runtime code reads the live override through the registration owner. It is
        // not part of the application-facing diagnostics view.
        public ZLinkMessageFlowLogMode effectiveMessageFlow() {
            AtomicReference<ZLinkMessageFlowLogMode> cell = liveMode;
            return cell != null ? cell.get() : messageFlow;
        }

        public AtomicReference<ZLinkMessageFlowLogMode> liveMode() {
            return liveMode;
        }

        // Install the shared live-mode cell (host wiring for the runtime toggle).
        public void installLiveMode(
            AtomicReference<ZLinkMessageFlowLogMode> cell) {
            liveMode = cell;
        }

        public void setMessageFlow(ZLinkMessageFlowLogMode mode) {
            messageFlow = requireNonNull(mode, "messageFlow");
        }

        public void setSampleRate(double sampleRate) {
            this.sampleRate = sampleRate;
        }

        public void setIncludeMessageSizes(boolean enabled) {
            includeMessageSizes = enabled;
        }

    }
}
