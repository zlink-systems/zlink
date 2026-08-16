using System.Globalization;

namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkRawRequestSubmitter
{
    public static async ValueTask<IReadOnlyList<Message>> SubmitAsync(
        IReadOnlyList<Message> parts,
        Func<IReadOnlyList<Message>, TimeSpan, CancellationToken,
            Task<IReadOnlyList<Message>>> request,
        TimeSpan timeout,
        string failureMessage,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(parts);
        ArgumentNullException.ThrowIfNull(request);
        try
        {
            return await request(parts, timeout, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (ZLinkRequestTerminalException terminal)
        {
            throw ZLinkRequestFailureMapper.CreateCompletionException(
                terminal.Result,
                terminal.FailureErrno,
                string.Format(
                    CultureInfo.InvariantCulture,
                    failureMessage,
                    terminal.Result));
        }
        catch (ZlinkRequestException error)
        {
            throw ZLinkRequestFailureMapper.CreateCompletionException(
                (RequestResult)(int)error.Result,
                string.Format(
                    CultureInfo.InvariantCulture,
                    failureMessage,
                    error.Result));
        }
        catch (ZlinkSubmitException error)
        {
            throw ZLinkRequestFailureMapper.CreateSubmitException(
                error,
                string.Format(
                    CultureInfo.InvariantCulture,
                    failureMessage,
                    error.Result));
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }
}
