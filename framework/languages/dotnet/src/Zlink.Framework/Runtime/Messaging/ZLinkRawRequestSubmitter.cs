namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkRawRequestSubmitter
{
    public static async ValueTask<IReadOnlyList<Message>> SubmitAsync<TState>(
        TState state,
        IReadOnlyList<Message> parts,
        Func<
            TState,
            IReadOnlyList<Message>,
            TimeSpan,
            CancellationToken,
            Task<IReadOnlyList<Message>>
        > request,
        TimeSpan timeout,
        Func<TState, object, string> failureMessage,
        CancellationToken cancellationToken
    )
    {
        ArgumentNullException.ThrowIfNull(parts);
        ArgumentNullException.ThrowIfNull(request);
        try
        {
            return await SubmitAsync(
                    (
                        State: state,
                        Parts: parts,
                        Request: request,
                        Timeout: timeout,
                        Token: cancellationToken,
                        FailureMessage: failureMessage
                    ),
                    static pending =>
                        pending.Request(
                            pending.State,
                            pending.Parts,
                            pending.Timeout,
                            pending.Token
                        ),
                    static (pending, result) => pending.FailureMessage(pending.State, result)
                )
                .ConfigureAwait(false);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    internal static async ValueTask<IReadOnlyList<Message>> SubmitAsync<TState>(
        TState state,
        Func<TState, Task<IReadOnlyList<Message>>> request,
        Func<TState, object, string> failureMessage
    )
    {
        ArgumentNullException.ThrowIfNull(request);
        try
        {
            Task<IReadOnlyList<Message>> completion;
            try
            {
                completion = request(state);
            }
            catch (ZlinkSubmitException error)
            {
                throw ZLinkRequestFailureMapper.CreateSubmitException(
                    error,
                    failureMessage(state, error.Result),
                    completionFailure: false
                );
            }
            return await completion.ConfigureAwait(false);
        }
        catch (ZLinkRequestTerminalException terminal)
        {
            throw ZLinkRequestFailureMapper.CreateCompletionException(
                terminal.Result,
                terminal.FailureErrno,
                failureMessage(state, terminal.Result)
            );
        }
        catch (ZlinkRequestException error)
        {
            throw ZLinkRequestFailureMapper.CreateCompletionException(
                (RequestResult)(int)error.Result,
                failureMessage(state, error.Result)
            );
        }
        catch (ZlinkSubmitException error)
        {
            throw ZLinkRequestFailureMapper.CreateSubmitException(
                error,
                failureMessage(state, error.Result)
            );
        }
    }
}
