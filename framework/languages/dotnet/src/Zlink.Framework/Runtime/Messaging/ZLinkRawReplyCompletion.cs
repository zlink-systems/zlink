namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkRawReplyCompletion
{
    public static void Complete(
        RequestResult result,
        IReadOnlyList<Message> reply,
        Action<IReadOnlyList<Message>> complete,
        Action<Exception> fail,
        string operationName)
    {
        if (result == RequestResult.Ok)
        {
            complete(reply);
            return;
        }

        ZLinkMessageParts.DisposeAll(reply);
        fail(ZLinkRequestFailureMapper.CreateCompletionException(result, operationName));
    }
}
