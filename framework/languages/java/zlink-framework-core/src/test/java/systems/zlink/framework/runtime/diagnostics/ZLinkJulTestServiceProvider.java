package systems.zlink.framework.runtime.diagnostics;

import org.slf4j.ILoggerFactory;
import org.slf4j.IMarkerFactory;
import org.slf4j.Marker;
import org.slf4j.event.Level;
import org.slf4j.event.LoggingEvent;
import org.slf4j.helpers.AbstractLogger;
import org.slf4j.helpers.BasicMarkerFactory;
import org.slf4j.helpers.MessageFormatter;
import org.slf4j.helpers.NOPMDCAdapter;
import org.slf4j.spi.LoggingEventAware;
import org.slf4j.spi.MDCAdapter;
import org.slf4j.spi.SLF4JServiceProvider;

import java.time.Instant;
import java.util.logging.LogRecord;

/** Standard test backend preserving the JUL observers used by existing runtime fixtures. */
public final class ZLinkJulTestServiceProvider implements SLF4JServiceProvider {
    private final ILoggerFactory loggerFactory = JulLogger::new;
    private final IMarkerFactory markerFactory = new BasicMarkerFactory();
    private final MDCAdapter mdcAdapter = new NOPMDCAdapter();

    @Override
    public ILoggerFactory getLoggerFactory() {
        return loggerFactory;
    }

    @Override
    public IMarkerFactory getMarkerFactory() {
        return markerFactory;
    }

    @Override
    public MDCAdapter getMDCAdapter() {
        return mdcAdapter;
    }

    @Override
    public String getRequestedApiVersion() {
        return "2.0.99";
    }

    @Override
    public void initialize() {}

    private static final class JulLogger extends AbstractLogger implements LoggingEventAware {
        private static final long serialVersionUID = 1L;
        private final java.util.logging.Logger destination;

        private JulLogger(String name) {
            this.name = name;
            destination = java.util.logging.Logger.getLogger(name);
        }

        @Override
        protected String getFullyQualifiedCallerName() {
            return JulLogger.class.getName();
        }

        @Override
        protected void handleNormalizedLoggingCall(
                Level level, Marker marker, String message, Object[] arguments, Throwable failure) {
            deliver(level, message, arguments, failure, Instant.now());
        }

        @Override
        public void log(LoggingEvent event) {
            Instant observed = Instant.ofEpochMilli(event.getTimeStamp());
            if (event.getKeyValuePairs() != null) {
                for (var field : event.getKeyValuePairs()) {
                    if (ZLinkTraceFormat.TIMESTAMP_FIELD.equals(field.key)
                            && field.value instanceof Instant timestamp) {
                        observed = timestamp;
                    }
                }
            }
            deliver(
                    event.getLevel(),
                    event.getMessage(),
                    event.getArgumentArray(),
                    event.getThrowable(),
                    observed);
        }

        private void deliver(
                Level level,
                String message,
                Object[] arguments,
                Throwable failure,
                Instant observed) {
            var record =
                    new LogRecord(
                            julLevel(level),
                            MessageFormatter.arrayFormat(message, arguments, failure).getMessage());
            record.setLoggerName(name);
            record.setThrown(failure);
            record.setInstant(observed);
            destination.log(record);
        }

        private boolean enabled(Level level) {
            return destination.isLoggable(julLevel(level));
        }

        private static java.util.logging.Level julLevel(Level level) {
            return switch (level) {
                case TRACE -> java.util.logging.Level.FINEST;
                case DEBUG -> java.util.logging.Level.FINE;
                case INFO -> java.util.logging.Level.INFO;
                case WARN -> java.util.logging.Level.WARNING;
                case ERROR -> java.util.logging.Level.SEVERE;
            };
        }

        @Override
        public boolean isTraceEnabled() {
            return enabled(Level.TRACE);
        }

        @Override
        public boolean isTraceEnabled(Marker marker) {
            return isTraceEnabled();
        }

        @Override
        public boolean isDebugEnabled() {
            return enabled(Level.DEBUG);
        }

        @Override
        public boolean isDebugEnabled(Marker marker) {
            return isDebugEnabled();
        }

        @Override
        public boolean isInfoEnabled() {
            return enabled(Level.INFO);
        }

        @Override
        public boolean isInfoEnabled(Marker marker) {
            return isInfoEnabled();
        }

        @Override
        public boolean isWarnEnabled() {
            return enabled(Level.WARN);
        }

        @Override
        public boolean isWarnEnabled(Marker marker) {
            return isWarnEnabled();
        }

        @Override
        public boolean isErrorEnabled() {
            return enabled(Level.ERROR);
        }

        @Override
        public boolean isErrorEnabled(Marker marker) {
            return isErrorEnabled();
        }
    }
}
