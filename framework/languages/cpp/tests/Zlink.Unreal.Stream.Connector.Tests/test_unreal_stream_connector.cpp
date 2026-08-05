/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "ZLinkStreamConnector.h"

#include <string>

int main ()
{
    UZLinkStreamConnector connector;
    connector.Connect ("tcp://127.0.0.1:9400");
    connector.Dispatch ();
    if (connector.IsConnected ()
        || connector.LastState () != EZLinkStreamConnectionState::Disconnected) {
        return 1;
    }
    if (connector.OnConnectionStateChanged.NumBroadcasts () == 0
        || connector.OnConnectionStateChanged.LastBroadcastValue ()
             != EZLinkStreamConnectionState::Disconnected) {
        return 6;
    }

    connector.SendJson ("chat.send", "{\"text\":\"hello\"}");
    FZLinkStreamSendOptions send_options;
    send_options.Metadata.emplace ("traceId", "send-1");
    send_options.bCompress = true;
    connector.SendJsonWithOptions ("chat.send.metadata", "{\"text\":\"hello\"}", send_options);
    connector.RequestJson ("chat.request", "{\"text\":\"hello\"}", 0.01f);
    FZLinkStreamSendOptions request_options;
    request_options.Metadata.emplace ("traceId", "request-1");
    request_options.bCompress = true;
    connector.RequestJsonWithOptions ("chat.request.metadata", "{\"text\":\"hello\"}", 0.01f,
                                      request_options);
    int request_completed_count = 0;
    connector.OnRequestCompletedNative.AddLambda (
      [&request_completed_count] (const FZLinkStreamPacket &) { ++request_completed_count; });
    connector.Dispatch ();
    connector.Tick (0.016f);
    if (connector.PendingDispatchCount () != 0) {
        return 2;
    }

    connector.ShutdownForPie ();
    connector.Dispatch ();
    if (connector.IsConnected () || connector.LastState () != EZLinkStreamConnectionState::Closed) {
        return 3;
    }
    if (connector.OnConnectionStateChanged.LastBroadcastValue ()
        != EZLinkStreamConnectionState::Closed) {
        return 7;
    }

    connector.Connect ("tcp://127.0.0.1:9401");
    connector.ShutdownForMapUnload ();
    connector.Dispatch ();
    if (connector.LastState () != EZLinkStreamConnectionState::Closed) {
        return 4;
    }

    connector.Connect ("tcp://127.0.0.1:9402");
    connector.ShutdownForGameInstanceShutdown ();
    connector.Dispatch ();
    if (connector.LastState () != EZLinkStreamConnectionState::Closed) {
        return 5;
    }

    return 0;
}
