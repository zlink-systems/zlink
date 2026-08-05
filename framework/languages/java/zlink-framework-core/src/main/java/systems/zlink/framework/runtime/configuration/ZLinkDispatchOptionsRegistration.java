package systems.zlink.framework.runtime.configuration;

import systems.zlink.framework.configuration.ZLinkDiagnosticsOptions;
import systems.zlink.framework.configuration.ZLinkDispatchOptions;
import systems.zlink.framework.configuration.ZLinkLogLevel;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMessageFlowObserver;
import systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction;
import systems.zlink.framework.configuration.ZLinkUnhandledDispatchOptions;
import systems.zlink.framework.errors.ZLinkConfigurationException;

public final class ZLinkDispatchOptionsRegistration implements ZLinkDispatchOptions {
    private final UnhandledDispatchOptions unhandled = new UnhandledDispatchOptions();
    private final DiagnosticsOptions diagnostics = new DiagnosticsOptions();
    private Class<? extends ZLinkMessageFlowObserver> messageFlowObserverType;
    private ZLinkMessageFlowObserver messageFlowObserver;

    @Override
    public UnhandledDispatchOptions unhandled() {
        return unhandled;
    }

    @Override
    public DiagnosticsOptions diagnostics() {
        return diagnostics;
    }

    public Class<? extends ZLinkMessageFlowObserver> messageFlowObserverType() {
        return messageFlowObserverType;
    }

    public ZLinkMessageFlowObserver messageFlowObserver() {
        return messageFlowObserver;
    }

    public ZLinkDispatchOptions setMessageFlowObserver(
        Class<? extends ZLinkMessageFlowObserver> observerType) {
        messageFlowObserverType = requireNonNull(observerType, "messageFlowObserverType");
        messageFlowObserver = null;
        return this;
    }

    @Override
    public ZLinkDispatchOptions setMessageFlowObserver(ZLinkMessageFlowObserver observer) {
        messageFlowObserver = requireNonNull(observer, "messageFlowObserver");
        messageFlowObserverType = null;
        return this;
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

    @Override
    public ZLinkDispatchOptions traceLogFile(String path) {
        diagnostics.setLogFile(path);
        return this;
    }

    @Override
    public ZLinkDispatchOptions traceLabel(String label) {
        diagnostics.setLabel(label);
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
        private ZLinkMessageFlowLogMode messageFlow = ZLinkMessageFlowLogMode.ERRORS_ONLY;
        private double sampleRate = 1.0d;
        private boolean includeMessageSizes = true;
        private boolean includeNativeDiagnostics;
        private String logFile;
        private String label;
        // Shared, runtime-mutable mode cell installed by the host at start; shared
        // across surfaces so setMessageFlowMode flips it live. Null before install.
        private volatile java.util.concurrent.atomic.AtomicReference<ZLinkMessageFlowLogMode> liveMode;

        public ZLinkMessageFlowLogMode messageFlow() {
            return messageFlow;
        }

        public double sampleRate() {
            return sampleRate;
        }

        public boolean includeMessageSizes() {
            return includeMessageSizes;
        }

        public boolean includeNativeDiagnostics() {
            return includeNativeDiagnostics;
        }

        public String logFile() {
            return logFile;
        }

        public String label() {
            return label;
        }

        public ZLinkMessageFlowLogMode effectiveMessageFlow() {
            java.util.concurrent.atomic.AtomicReference<ZLinkMessageFlowLogMode> cell = liveMode;
            return cell != null ? cell.get() : messageFlow;
        }

        public java.util.concurrent.atomic.AtomicReference<ZLinkMessageFlowLogMode> liveMode() {
            return liveMode;
        }

        // Install the shared live-mode cell (host wiring for the runtime toggle).
        public void installLiveMode(
            java.util.concurrent.atomic.AtomicReference<ZLinkMessageFlowLogMode> cell) {
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

        public void setIncludeNativeDiagnostics(boolean enabled) {
            includeNativeDiagnostics = enabled;
        }

        public void setLogFile(String path) {
            logFile = path == null || path.isBlank() ? null : path;
        }

        public void setLabel(String label) {
            this.label = label == null || label.isBlank() ? null : label;
        }
    }
}
