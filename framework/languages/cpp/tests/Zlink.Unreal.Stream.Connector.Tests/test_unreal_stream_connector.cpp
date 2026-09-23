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
    int request_completed_count = 0;
    connector.RequestJson ("chat.request", "{\"text\":\"hello\"}", 0.01f,
                           [&request_completed_count] (const FZLinkStreamRequestResult &) {
                               ++request_completed_count;
                           });
    FZLinkStreamSendOptions request_options;
    request_options.Metadata.emplace ("traceId", "request-1");
    request_options.bCompress = true;
    connector.RequestJsonWithOptions (
      "chat.request.metadata", "{\"text\":\"hello\"}", 0.01f, request_options,
      [&request_completed_count] (const FZLinkStreamRequestResult &) {
          ++request_completed_count;
      });
    connector.Dispatch ();
    connector.Tick (0.016f);
    if (connector.PendingDispatchCount () != 0) {
        return 2;
    }

    int reconnect_failures = 0;
    int reconnect_completions = 0;
    auto reply_hook =
      connector.OnReplyReceived ([&] (const FZLinkStreamReplyReceivedContext &context) {
          if (context.RequestPacketName == "chat.reconnect" && !context.bSucceeded
              && context.bHasError) {
              ++reconnect_failures;
          }
      });
    connector.RequestJson ("chat.reconnect", "{}", 0.01f,
                           [&] (const FZLinkStreamRequestResult &result) {
                               if (!result.bSuccess) {
                                   ++reconnect_completions;
                               }
                           });
    connector.Connect ("tcp://127.0.0.1:9403");
    connector.Dispatch ();
    if (reconnect_failures != 1 || reconnect_completions != 1) {
        return 8;
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
