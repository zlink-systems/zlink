import { logs } from '@opentelemetry/api-logs';

// Loaded before the application's standard preloads in this worker only. The
// public API returns this proxy until a logger provider is registered, then the
// registered provider itself. No provider is changed or called to probe it.
export const initialLoggerProvider = logs.getLoggerProvider();
