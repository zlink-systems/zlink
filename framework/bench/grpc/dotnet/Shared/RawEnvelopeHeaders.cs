using System.Text;

namespace WithGrpcBench.Shared;

public static class RawEnvelopeHeaders
{
    public static readonly byte[] Request = Encoding.UTF8.GetBytes(
        "{\"kind\":1,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\",\"contentType\":\"application/x-protobuf\",\"correlationId\":null,\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}"
    );

    public static readonly byte[] Response = Encoding.UTF8.GetBytes(
        "{\"kind\":2,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\",\"contentType\":\"application/x-protobuf\",\"correlationId\":null,\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}"
    );

    public static readonly byte[] JsonRequest = Encoding.UTF8.GetBytes(
        "{\"kind\":1,\"channelName\":\"bench\",\"messageName\":\"BenchGameMessage\",\"contentType\":\"application/json\",\"correlationId\":null,\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}"
    );

    public static readonly byte[] JsonResponse = Encoding.UTF8.GetBytes(
        "{\"kind\":2,\"channelName\":\"bench\",\"messageName\":\"BenchGameMessage\",\"contentType\":\"application/json\",\"correlationId\":null,\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}"
    );
}
