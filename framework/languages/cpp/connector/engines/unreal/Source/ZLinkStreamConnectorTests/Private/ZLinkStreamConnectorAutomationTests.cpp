/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#if __has_include("CoreMinimal.h")

#include "ZLinkStreamConnector.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST (FZLinkStreamConnectorLifecycleTest,
                                  "ZLink.StreamConnector.Lifecycle",
                                  EAutomationTestFlags::EditorContext
                                    | EAutomationTestFlags::EngineFilter)

bool FZLinkStreamConnectorLifecycleTest::RunTest (const FString &)
{
    UZLinkStreamConnector *Connector = NewObject<UZLinkStreamConnector> ();
    TestNotNull (TEXT ("connector object"), Connector);

    Connector->Connect (TEXT ("tcp://127.0.0.1:1"));
    Connector->Dispatch ();
    TestFalse (TEXT ("unreachable endpoint is not connected"), Connector->IsConnected ());

    int SendingCalls = 0;
    int ReplyCalls = 0;
    auto Sending =
      Connector->OnRequestSending ([&SendingCalls] (FZLinkStreamRequestSendingContext &Context) {
          ++SendingCalls;
          Context.SetMetadata (TEXT ("unreal-hook"), TEXT ("yes"));
      });
    auto Reply =
      Connector->OnReplyReceived ([&ReplyCalls] (const FZLinkStreamReplyReceivedContext &Context) {
          if (!Context.bSucceeded && Context.bHasError) {
              ++ReplyCalls;
          }
      });

    Connector->SendJson (TEXT ("chat.send"), TEXT ("{\"text\":\"hello\"}"));
    Connector->RequestJson (TEXT ("chat.request"), TEXT ("{\"text\":\"hello\"}"), 0.01f);
    TestEqual (TEXT ("sending hook runs in request call"), SendingCalls, 1);
    TestEqual (TEXT ("reply hook waits for dispatch"), ReplyCalls, 0);
    Connector->Dispatch ();

    Connector->ShutdownForPie ();
    Connector->Dispatch ();
    TestEqual (TEXT ("PIE shutdown closes connector"), Connector->LastState (),
               EZLinkStreamConnectionState::Closed);

    Connector->Connect (TEXT ("tcp://127.0.0.1:1"));
    Connector->ShutdownForMapUnload ();
    Connector->Dispatch ();
    TestEqual (TEXT ("map unload closes connector"), Connector->LastState (),
               EZLinkStreamConnectionState::Closed);

    Connector->Connect (TEXT ("tcp://127.0.0.1:1"));
    Connector->ShutdownForGameInstanceShutdown ();
    Connector->Dispatch ();
    TestEqual (TEXT ("game instance shutdown closes connector"), Connector->LastState (),
               EZLinkStreamConnectionState::Closed);
    return true;
}

#endif
#endif
