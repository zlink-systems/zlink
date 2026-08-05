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

    Connector->SendJson (TEXT ("chat.send"), TEXT ("{\"text\":\"hello\"}"));
    Connector->RequestJson (TEXT ("chat.request"), TEXT ("{\"text\":\"hello\"}"), 0.01f);
    Connector->Dispatch ();

    Connector->ShutdownForPie ();
    Connector->Dispatch ();
    TestEqual (TEXT ("PIE shutdown closes connector"),
               Connector->LastState (),
               EZLinkStreamConnectionState::Closed);

    Connector->Connect (TEXT ("tcp://127.0.0.1:1"));
    Connector->ShutdownForMapUnload ();
    Connector->Dispatch ();
    TestEqual (TEXT ("map unload closes connector"),
               Connector->LastState (),
               EZLinkStreamConnectionState::Closed);

    Connector->Connect (TEXT ("tcp://127.0.0.1:1"));
    Connector->ShutdownForGameInstanceShutdown ();
    Connector->Dispatch ();
    TestEqual (TEXT ("game instance shutdown closes connector"),
               Connector->LastState (),
               EZLinkStreamConnectionState::Closed);
    return true;
}

#endif
#endif
