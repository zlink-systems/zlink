/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "ZLinkStreamConnector.h"

#include <zlink/stream_connector.hpp>

#if __has_include("CoreMinimal.h")
#include "UObject/WeakObjectPtr.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace UE::ZLinkStreamConnector::Private
{

namespace
{

std::string to_utf8 (const FString &value)
{
#if __has_include("CoreMinimal.h")
    const FTCHARToUTF8 converted (*value);
    return std::string (converted.Get (), converted.Get () + converted.Length ());
#else
    return value;
#endif
}

zlink::stream_connector::metadata_t to_metadata (const TMap<FString, FString> &metadata)
{
    zlink::stream_connector::metadata_t converted;
#if __has_include("CoreMinimal.h")
    for (const auto &entry : metadata) {
        converted.with (to_utf8 (entry.Key), to_utf8 (entry.Value));
    }
#else
    for (const auto &entry : metadata) {
        converted.with (entry.first, entry.second);
    }
#endif
    return converted;
}

zlink::message_t to_payload (const FString &json)
{
    return zlink::message_t::from (to_utf8 (json));
}

EZLinkStreamConnectionState to_unreal_state (zlink::stream_connector::connection_state_t state)
{
    switch (state) {
        case zlink::stream_connector::connection_state_t::connecting:
            return EZLinkStreamConnectionState::Connecting;
        case zlink::stream_connector::connection_state_t::connected:
            return EZLinkStreamConnectionState::Connected;
        case zlink::stream_connector::connection_state_t::reconnecting:
            return EZLinkStreamConnectionState::Reconnecting;
        case zlink::stream_connector::connection_state_t::disconnected:
            return EZLinkStreamConnectionState::Disconnected;
        case zlink::stream_connector::connection_state_t::closed:
            return EZLinkStreamConnectionState::Closed;
        case zlink::stream_connector::connection_state_t::created:
        default:
            return EZLinkStreamConnectionState::Created;
    }
}

} // namespace

class FZLinkStreamConnectorRuntime
{
  public:
    explicit FZLinkStreamConnectorRuntime (UZLinkStreamConnector &Owner) :
#if __has_include("CoreMinimal.h")
        Pending (std::make_shared<pending_state_t> (&Owner))
#else
        Pending (std::make_shared<pending_state_t> (&Owner))
#endif
    {
    }

    void DetachOwner ()
    {
        PacketSubscriptions.clear ();
        StateSubscription.unsubscribe ();
        Connector.close ();
        std::lock_guard<std::mutex> lock (Pending->Mutex);
#if __has_include("CoreMinimal.h")
        Pending->Owner.Reset ();
#else
        Pending->Owner = nullptr;
#endif
        Pending->CancelCallbacks = true;
        Pending->Packets.clear ();
        Pending->Requests.clear ();
        Pending->States.clear ();
        Pending->LastConnectionState = EZLinkStreamConnectionState::Closed;
    }

    void Connect (const FString &Endpoint)
    {
        for (auto &subscription : PacketSubscriptions) {
            subscription.second.Handle.unsubscribe ();
        }
        StateSubscription.unsubscribe ();
        Connector.close ();
        {
            std::lock_guard<std::mutex> lock (Pending->Mutex);
            Pending->CancelCallbacks = false;
            Pending->Packets.clear ();
            Pending->Requests.clear ();
            Pending->States.clear ();
        }
        zlink::stream_connector::connector_options_t options;
        options.endpoint = to_utf8 (Endpoint);
        options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
        Connector = zlink::stream_connector::connector_factory_t::create (std::move (options));
        for (auto &subscription : PacketSubscriptions) {
            subscription.second.Handle = RegisterPacket (subscription.first);
        }
        auto pending = Pending;
        /* stream-connector §7: the handle owns the registration, so it is kept
         * for as long as the runtime needs the handler. */
        StateSubscription = Connector.on_connection_state_changed (
          [pending] (const zlink::stream_connector::connection_state_changed_t &event) {
              EnqueueState (pending, to_unreal_state (event.current));
          });

        SetLastState (EZLinkStreamConnectionState::Connecting);
        EnqueueState (Pending, EZLinkStreamConnectionState::Connecting);

        const auto connected = Connector.connect ();
        if (connected) {
            SetLastState (EZLinkStreamConnectionState::Connected);
            EnqueueState (Pending, EZLinkStreamConnectionState::Connected);
            return;
        }

        SetLastState (EZLinkStreamConnectionState::Disconnected);
        EnqueueState (Pending, EZLinkStreamConnectionState::Disconnected);
    }

    void Close ()
    {
        PacketSubscriptions.clear ();
        StateSubscription.unsubscribe ();
        Connector.close ();
        {
            std::lock_guard<std::mutex> lock (Pending->Mutex);
            Pending->CancelCallbacks = true;
            Pending->Packets.clear ();
            Pending->Requests.clear ();
            Pending->States.clear ();
            Pending->LastConnectionState = EZLinkStreamConnectionState::Closed;
        }
        EnqueueState (Pending, EZLinkStreamConnectionState::Closed);
    }

    FZLinkStreamSubscriptionHandle On (const FName &PacketName,
                                       TFunction<void (const FZLinkStreamPacket &)> Callback)
    {
#if __has_include("CoreMinimal.h")
        const auto name = to_utf8 (PacketName.ToString ());
#else
        const auto name = to_utf8 (PacketName);
#endif
        const auto id = NextSubscriptionId++;
        auto &subscription = PacketSubscriptions.emplace (id, subscription_entry_t{}).first->second;
        subscription.Name = name;
        subscription.Callback =
          std::make_shared<TFunction<void (const FZLinkStreamPacket &)>> (std::move (Callback));
        if (!Connector.options ().endpoint.empty ()) {
            subscription.Handle = RegisterPacket (id);
        }
        return {id};
    }

    void Unsubscribe (FZLinkStreamSubscriptionHandle Handle)
    {
        PacketSubscriptions.erase (Handle.Id);
    }

    zlink::stream_connector::subscription_t RegisterPacket (int32 id)
    {
        auto pending = Pending;
        return Connector.on<zlink::stream_connector::packet_t> (
          PacketSubscriptions.at (id).Name,
          [pending, id] (
            const zlink::stream_connector::message_t<zlink::stream_connector::packet_t> &message) {
              FZLinkStreamPacket packet = ToUnrealPacket (message.payload);
#if __has_include("CoreMinimal.h")
              packet.PacketName = FName (UTF8_TO_TCHAR (message.packet_name.c_str ()));
#else
              packet.PacketName = message.packet_name;
#endif
              std::lock_guard<std::mutex> lock (pending->Mutex);
              if (!pending->CancelCallbacks) {
                  pending->Packets.emplace_back (id, std::move (packet));
              }
          });
    }

    void SendJson (const FName &PacketName, const FString &JsonPayload)
    {
        SendJson (PacketName, JsonPayload, FZLinkStreamSendOptions{});
    }

    void SendJson (const FName &PacketName,
                   const FString &JsonPayload,
                   const FZLinkStreamSendOptions &Options)
    {
        auto call = Connector.send (MakePacket (PacketName, JsonPayload, Options));
        if (Options.bCompress) {
            call.compress ();
        }
        call.submit ();
    }

    void RequestJson (const FName &PacketName,
                      const FString &JsonPayload,
                      float TimeoutSeconds,
                      TFunction<void (const FZLinkStreamRequestResult &)> Callback)
    {
        RequestJson (PacketName, JsonPayload, TimeoutSeconds, FZLinkStreamSendOptions{},
                     std::move (Callback));
    }

    void RequestJson (const FName &PacketName,
                      const FString &JsonPayload,
                      float TimeoutSeconds,
                      const FZLinkStreamSendOptions &Options,
                      TFunction<void (const FZLinkStreamRequestResult &)> Callback)
    {
        auto request = Connector.request (MakePacket (PacketName, JsonPayload, Options));
        request.timeout (
          std::chrono::milliseconds (std::max (1, static_cast<int> (TimeoutSeconds * 1000.0f))));
        if (Options.bCompress) {
            request.compress ();
        }
        auto pending = Pending;
        auto completion = std::make_shared<request_callback_t> (std::move (Callback));
        request.submit<zlink::message_t> (
          [pending, completion] (zlink::stream_connector::result_t<zlink::message_t> result) {
              FZLinkStreamRequestResult completed;
              completed.bSuccess = static_cast<bool> (result);
              if (result) {
                  zlink::stream_connector::packet_t reply;
                  reply.payload = std::move (result.value ());
                  completed.Packet = ToUnrealPacket (reply);
              } else if (result.error ()) {
                  completed.ErrorCode = static_cast<int32> (result.error ()->code);
#if __has_include("CoreMinimal.h")
                  completed.ErrorMessage = UTF8_TO_TCHAR (result.error ()->message.c_str ());
#else
                  completed.ErrorMessage = result.error ()->message;
#endif
              }
              std::lock_guard<std::mutex> lock (pending->Mutex);
              if (pending->CancelCallbacks) {
                  return;
              }
              pending->Requests.emplace_back (std::move (completion), std::move (completed));
          });
    }

    void Dispatch ()
    {
        Connector.dispatch ();
        std::vector<EZLinkStreamConnectionState> pending_states;
        std::vector<std::pair<int32, FZLinkStreamPacket>> pending_packets;
        std::vector<std::pair<std::shared_ptr<request_callback_t>, FZLinkStreamRequestResult>>
          pending_requests;
        bool cancel_callbacks = false;
        UZLinkStreamConnector *owner = nullptr;
        {
            std::lock_guard<std::mutex> lock (Pending->Mutex);
            owner = Owner ();
            cancel_callbacks = Pending->CancelCallbacks;
            pending_states.swap (Pending->States);
            if (!cancel_callbacks) {
                pending_packets.swap (Pending->Packets);
                pending_requests.swap (Pending->Requests);
            } else {
                Pending->Packets.clear ();
                Pending->Requests.clear ();
            }
        }
        if (!owner) {
            return;
        }

        for (const auto state : pending_states) {
            {
                std::lock_guard<std::mutex> lock (Pending->Mutex);
                Pending->LastConnectionState = state;
            }
            owner->OnConnectionStateChanged.Broadcast (state);
        }
        if (cancel_callbacks) {
            return;
        }
        for (auto &packet : pending_packets) {
            const auto found = PacketSubscriptions.find (packet.first);
            if (found != PacketSubscriptions.end ()) {
                auto callback = found->second.Callback;
                if (callback && *callback) {
                    (*callback) (packet.second);
                }
            }
        }
        for (auto &request : pending_requests) {
            if (*request.first) {
                (*request.first) (request.second);
            }
        }
    }

    bool IsConnected () const { return Connector.is_connected (); }

    int PendingDispatchCount () const
    {
        std::size_t local_count = 0;
        {
            std::lock_guard<std::mutex> lock (Pending->Mutex);
            local_count =
              Pending->States.size () + Pending->Packets.size () + Pending->Requests.size ();
        }
        return static_cast<int> (local_count + Connector.pending_dispatch_count ());
    }

    EZLinkStreamConnectionState LastState () const
    {
        std::lock_guard<std::mutex> lock (Pending->Mutex);
        return Pending->LastConnectionState;
    }

  private:
    using request_callback_t = TFunction<void (const FZLinkStreamRequestResult &)>;

    struct subscription_entry_t
    {
        std::string Name;
        std::shared_ptr<TFunction<void (const FZLinkStreamPacket &)>> Callback;
        zlink::stream_connector::subscription_t Handle;
    };
    zlink::stream_connector::packet_t MakePacket (const FName &PacketName,
                                                  const FString &JsonPayload,
                                                  const FZLinkStreamSendOptions &Options)
    {
        zlink::stream_connector::packet_t packet;
#if __has_include("CoreMinimal.h")
        packet.name = to_utf8 (PacketName.ToString ());
#else
        packet.name = PacketName;
#endif
        packet.codec = zlink::stream_connector::codec_t::json;
        packet.metadata = to_metadata (Options.Metadata);
        packet.payload = to_payload (JsonPayload);
        return packet;
    }

    static FZLinkStreamPacket ToUnrealPacket (const zlink::stream_connector::packet_t &packet)
    {
        FZLinkStreamPacket converted;
#if __has_include("CoreMinimal.h")
        converted.PacketName = FName (UTF8_TO_TCHAR (packet.name.c_str ()));
#else
        converted.PacketName = packet.name;
#endif
        const auto payload = packet.payload.to_string ();
#if __has_include("CoreMinimal.h")
        converted.Payload.Append (reinterpret_cast<const uint8 *> (payload.data ()),
                                  static_cast<int32> (payload.size ()));
#else
        converted.Payload.assign (payload.begin (), payload.end ());
#endif
#if __has_include("CoreMinimal.h")
        for (const auto &entry : packet.metadata.values) {
            converted.Metadata.Add (FString (UTF8_TO_TCHAR (entry.first.c_str ())),
                                    FString (UTF8_TO_TCHAR (entry.second.c_str ())));
        }
#else
        for (const auto &entry : packet.metadata.values) {
            converted.Metadata.emplace (entry.first, entry.second);
        }
#endif
        converted.bCompressed = false;
        return converted;
    }

    struct pending_state_t
    {
#if __has_include("CoreMinimal.h")
        explicit pending_state_t (UZLinkStreamConnector *owner) : Owner (owner) {}
        TWeakObjectPtr<UZLinkStreamConnector> Owner;
#else
        explicit pending_state_t (UZLinkStreamConnector *owner) : Owner (owner) {}
        UZLinkStreamConnector *Owner;
#endif
        mutable std::mutex Mutex;
        bool CancelCallbacks = false;
        EZLinkStreamConnectionState LastConnectionState = EZLinkStreamConnectionState::Created;
        std::vector<EZLinkStreamConnectionState> States;
        std::vector<std::pair<int32, FZLinkStreamPacket>> Packets;
        std::vector<std::pair<std::shared_ptr<request_callback_t>, FZLinkStreamRequestResult>>
          Requests;
    };

    static void EnqueueState (const std::shared_ptr<pending_state_t> &PendingState,
                              EZLinkStreamConnectionState State)
    {
        std::lock_guard<std::mutex> lock (PendingState->Mutex);
        PendingState->States.push_back (State);
    }

    void SetLastState (EZLinkStreamConnectionState State)
    {
        std::lock_guard<std::mutex> lock (Pending->Mutex);
        Pending->LastConnectionState = State;
    }

    UZLinkStreamConnector *Owner () const
    {
#if __has_include("CoreMinimal.h")
        return Pending->Owner.Get ();
#else
        return Pending->Owner;
#endif
    }

    zlink::stream_connector::connector_t Connector;
    zlink::stream_connector::subscription_t StateSubscription;
    std::map<int32, subscription_entry_t> PacketSubscriptions;
    int32 NextSubscriptionId = 1;
    std::shared_ptr<pending_state_t> Pending;
};

} // namespace UE::ZLinkStreamConnector::Private

UZLinkStreamConnector::UZLinkStreamConnector () :
    _runtime (
      std::make_unique<UE::ZLinkStreamConnector::Private::FZLinkStreamConnectorRuntime> (*this))
{
}

UZLinkStreamConnector::~UZLinkStreamConnector ()
{
    if (_runtime) {
        _runtime->DetachOwner ();
    }
}

void UZLinkStreamConnector::Connect (const FString &Endpoint)
{
    _runtime->Connect (Endpoint);
}

void UZLinkStreamConnector::Close ()
{
    _runtime->Close ();
}

FZLinkStreamSubscriptionHandle UZLinkStreamConnector::On (FName PacketName,
                                                          FZLinkStreamPacketDelegate Delegate)
{
    return On (PacketName,
               [Delegate] (const FZLinkStreamPacket &Packet) { Delegate.ExecuteIfBound (Packet); });
}

FZLinkStreamSubscriptionHandle
UZLinkStreamConnector::On (FName PacketName, TFunction<void (const FZLinkStreamPacket &)> Callback)
{
    return _runtime->On (PacketName, std::move (Callback));
}

void UZLinkStreamConnector::Unsubscribe (FZLinkStreamSubscriptionHandle Handle)
{
    _runtime->Unsubscribe (Handle);
}

void UZLinkStreamConnector::SendJson (FName PacketName, const FString &JsonPayload)
{
    _runtime->SendJson (PacketName, JsonPayload);
}

void UZLinkStreamConnector::SendJsonWithOptions (FName PacketName,
                                                 const FString &JsonPayload,
                                                 const FZLinkStreamSendOptions &Options)
{
    _runtime->SendJson (PacketName, JsonPayload, Options);
}

void UZLinkStreamConnector::RequestJson (FName PacketName,
                                         const FString &JsonPayload,
                                         float TimeoutSeconds,
                                         FZLinkStreamRequestDelegate OnCompleted)
{
    RequestJson (PacketName, JsonPayload, TimeoutSeconds,
                 [OnCompleted] (const FZLinkStreamRequestResult &Result) {
                     OnCompleted.ExecuteIfBound (Result);
                 });
}

void UZLinkStreamConnector::RequestJson (
  FName PacketName,
  const FString &JsonPayload,
  float TimeoutSeconds,
  TFunction<void (const FZLinkStreamRequestResult &)> OnCompleted)
{
    _runtime->RequestJson (PacketName, JsonPayload, TimeoutSeconds, std::move (OnCompleted));
}

void UZLinkStreamConnector::RequestJsonWithOptions (FName PacketName,
                                                    const FString &JsonPayload,
                                                    float TimeoutSeconds,
                                                    const FZLinkStreamSendOptions &Options,
                                                    FZLinkStreamRequestDelegate OnCompleted)
{
    RequestJsonWithOptions (PacketName, JsonPayload, TimeoutSeconds, Options,
                            [OnCompleted] (const FZLinkStreamRequestResult &Result) {
                                OnCompleted.ExecuteIfBound (Result);
                            });
}

void UZLinkStreamConnector::RequestJsonWithOptions (
  FName PacketName,
  const FString &JsonPayload,
  float TimeoutSeconds,
  const FZLinkStreamSendOptions &Options,
  TFunction<void (const FZLinkStreamRequestResult &)> OnCompleted)
{
    _runtime->RequestJson (PacketName, JsonPayload, TimeoutSeconds, Options,
                           std::move (OnCompleted));
}

void UZLinkStreamConnector::Dispatch ()
{
    _runtime->Dispatch ();
}

void UZLinkStreamConnector::Tick (float)
{
    _runtime->Dispatch ();
}

void UZLinkStreamConnector::ShutdownForPie ()
{
    _runtime->Close ();
}

void UZLinkStreamConnector::ShutdownForMapUnload ()
{
    _runtime->Close ();
}

void UZLinkStreamConnector::ShutdownForGameInstanceShutdown ()
{
    _runtime->Close ();
}

bool UZLinkStreamConnector::IsConnected () const
{
    return const_cast<UE::ZLinkStreamConnector::Private::FZLinkStreamConnectorRuntime *> (
             _runtime.get ())
      ->IsConnected ();
}

int UZLinkStreamConnector::PendingDispatchCount () const
{
    return const_cast<UE::ZLinkStreamConnector::Private::FZLinkStreamConnectorRuntime *> (
             _runtime.get ())
      ->PendingDispatchCount ();
}

EZLinkStreamConnectionState UZLinkStreamConnector::LastState () const
{
    return const_cast<UE::ZLinkStreamConnector::Private::FZLinkStreamConnectorRuntime *> (
             _runtime.get ())
      ->LastState ();
}
