using System.Globalization;

namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkRawRequestSubmitter
{
    public static async ValueTask<IReadOnlyList<Message>> SubmitAsync(
        ZLinkAsyncSubmitter submitter,
        Message message,
        Func<Message, RequestCallback, TimeSpan?, bool> request,
        TimeSpan timeout,
        string failureMessage,
        CancellationToken cancellationToken)
    {
        return await submitter.SubmitRequestAsync<IReadOnlyList<Message>>(
                message,
                (pending, complete, fail) => request(
                    pending,
                    CreateCompletion(complete, fail, failureMessage),
                    timeout),
                cancellationToken,
                ZLinkMessageParts.DisposeAll)
            .ConfigureAwait(false);
    }

    public static async ValueTask<IReadOnlyList<Message>> SubmitAsync(
        ZLinkAsyncSubmitter submitter,
        IReadOnlyList<Message> parts,
        Func<IReadOnlyList<Message>, RequestCallback, TimeSpan?, bool> request,
        TimeSpan timeout,
        string failureMessage,
        CancellationToken cancellationToken)
    {
        return await submitter.SubmitRequestAsync<IReadOnlyList<Message>>(
                parts,
                (pending, complete, fail) => request(
                    pending,
                    CreateCompletion(complete, fail, failureMessage),
                    timeout),
                cancellationToken,
                ZLinkMessageParts.DisposeAll)
            .ConfigureAwait(false);
    }

    private static RequestCallback CreateCompletion(
        Action<IReadOnlyList<Message>> complete,
        Action<Exception> fail,
        string failureMessage)
    {
        return (result, reply) => ZLinkRawReplyCompletion.Complete(
            result,
            reply,
            complete,
            fail,
            string.Format(CultureInfo.InvariantCulture, failureMessage, result));
    }
}
