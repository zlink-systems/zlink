/* SPDX-License-Identifier: Apache-2.0 */

namespace Zlink.HttpClient.Runtime;

internal enum HttpFailureStage
{
    Transport,
    Application,
}

internal static class HttpFailureMapper
{
    internal static Exception Map(Exception exception, HttpFailureStage stage)
    {
        if (exception is ZLinkFrameworkException)
            return exception;

        if (stage == HttpFailureStage.Transport)
        {
            if (exception is OperationCanceledException)
                return exception;
            if (
                exception is HttpRequestException
                {
                    InnerException: ZLinkFrameworkException classified
                }
            )
                return classified;
            return new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                exception.Message,
                ZLinkRetryAdvice.RetryAfterBackoff,
                exception
            );
        }

        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.InternalFailure,
            exception.Message,
            ZLinkRetryAdvice.DoNotRetry,
            exception
        );
    }
}
