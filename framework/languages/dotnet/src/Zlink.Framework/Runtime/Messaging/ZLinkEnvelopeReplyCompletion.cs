namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkEnvelopeReplyCompletion
{
    public static void Complete<TReply>(
        RequestResult result,
        IReadOnlyList<Message> reply,
        Action<TReply> complete,
        Action<Exception> fail,
        string operationName,
        ZLinkCodecRegistryBuilder? codecs = null)
    {
        try
        {
            if (result != RequestResult.Ok)
            {
                fail(ZLinkRequestFailureMapper.CreateCompletionException(result, operationName));
                return;
            }

            complete(ZLinkEnvelopeReplyDecoder.Decode<TReply>(
                reply,
                $"{operationName} reply is empty.",
                $"{operationName} failed.",
                codecs));
        }
        catch (Exception exception)
        {
            fail(exception);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(reply);
        }
    }
}
