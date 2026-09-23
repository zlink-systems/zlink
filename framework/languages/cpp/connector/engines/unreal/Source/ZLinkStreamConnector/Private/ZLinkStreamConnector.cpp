/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "ZLinkStreamConnector.h"

#include <zlink/stream_connector.hpp>

#if __has_include("CoreMinimal.h")
#include "UObject/WeakObjectPtr.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace UE::ZLinkStreamConnector::Private
{

static_assert (static_cast<int> (zlink::stream_connector::error_code_t::disconnected)
                 == static_cast<int> (EZLinkStreamErrorCode::Disconnected)
               && static_cast<int> (zlink::stream_connector::error_code_t::configuration_error)
                    == static_cast<int> (EZLinkStreamErrorCode::ConfigurationError)
               && static_cast<int> (zlink::stream_connector::error_code_t::validation_failed)
                    == static_cast<int> (EZLinkStreamErrorCode::ValidationFailed)
               && static_cast<int> (zlink::stream_connector::error_code_t::request_timeout)
                    == static_cast<int> (EZLinkStreamErrorCode::RequestTimeout)
               && static_cast<int> (zlink::stream_connector::error_code_t::connect_timeout)
                    == static_cast<int> (EZLinkStreamErrorCode::ConnectTimeout)
               && static_cast<int> (zlink::stream_connector::error_code_t::frame_decode_failed)
                    == static_cast<int> (EZLinkStreamErrorCode::FrameDecodeFailed)
               && static_cast<int> (zlink::stream_connector::error_code_t::frame_too_large)
                    == static_cast<int> (EZLinkStreamErrorCode::FrameTooLarge)
               && static_cast<int> (zlink::stream_connector::error_code_t::send_failed)
                    == static_cast<int> (EZLinkStreamErrorCode::SendFailed)
               && static_cast<int> (zlink::stream_connector::error_code_t::compression_failed)
                    == static_cast<int> (EZLinkStreamErrorCode::CompressionFailed)
               && static_cast<int> (zlink::stream_connector::error_code_t::tls_validation_failed)
                    == static_cast<int> (EZLinkStreamErrorCode::TlsValidationFailed)
               && static_cast<int> (zlink::stream_connector::error_code_t::decompression_failed)
                    == static_cast<int> (EZLinkStreamErrorCode::DecompressionFailed)
               && static_cast<int> (zlink::stream_connector::error_code_t::user_callback_failed)
                    == static_cast<int> (EZLinkStreamErrorCode::UserCallbackFailed)
               && static_cast<int> (zlink::stream_connector::error_code_t::remote_error)
                    == static_cast<int> (EZLinkStreamErrorCode::RemoteError));

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

FString from_utf8 (const std::string &value)
{
#if __has_include("CoreMinimal.h")
    return FString (UTF8_TO_TCHAR (value.c_str ()));
#else
    return value;
#endif
}

FName to_name (const std::string &value)
{
#if __has_include("CoreMinimal.h")
    return FName (UTF8_TO_TCHAR (value.c_str ()));
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
        {
            std::lock_guard<std::mutex> lock (Hooks->Mutex);
            Hooks->Sending.clear ();
            Hooks->Reply.clear ();
        }
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
        Pending->Replies.clear ();
        Pending->States.clear ();
        Pending->LastConnectionState = EZLinkStreamConnectionState::Closed;
    }

    void Connect (const FString &Endpoint)
    {
        std::lock_guard<std::mutex> hook_lock (Hooks->Mutex);
        for (auto &subscription : PacketSubscriptions) {
            subscription.second.unsubscribe ();
        }
        StateSubscription.unsubscribe ();
        Connector.close ();
        while (Connector.pending_dispatch_count () > 0) {
            Connector.dispatch ();
        }
        for (auto &[id, entry] : Hooks->Sending) {
            entry.Subscription.unsubscribe ();
        }
        for (auto &[id, entry] : Hooks->Reply) {
            entry.Subscription.unsubscribe ();
        }
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
        for (auto &[id, entry] : Hooks->Sending) {
            entry.Subscription = RegisterSending (entry.Callback);
        }
        for (auto &[id, entry] : Hooks->Reply) {
            entry.Subscription = RegisterReply (id);
        }
        for (auto &subscription : PacketSubscriptions) {
            subscription.second = RegisterPacket (subscription.first);
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

    void Subscribe (const FName &PacketName)
    {
#if __has_include("CoreMinimal.h")
        const auto name = to_utf8 (PacketName.ToString ());
#else
        const auto name = to_utf8 (PacketName);
#endif
        auto &subscription =
          PacketSubscriptions.emplace_back (name, zlink::stream_connector::subscription_t{});
        if (!Connector.options ().endpoint.empty ()) {
            subscription.second = RegisterPacket (name);
        }
    }

    zlink::stream_connector::subscription_t RegisterPacket (const std::string &name)
    {
        auto pending = Pending;
        return Connector.on<zlink::stream_connector::packet_t> (
          name,
          [pending] (
            const zlink::stream_connector::message_t<zlink::stream_connector::packet_t> &message) {
              FZLinkStreamPacket packet = ToUnrealPacket (message.payload);
#if __has_include("CoreMinimal.h")
              packet.PacketName = FName (UTF8_TO_TCHAR (message.packet_name.c_str ()));
#else
              packet.PacketName = message.packet_name;
#endif
              std::lock_guard<std::mutex> lock (pending->Mutex);
              if (!pending->CancelCallbacks) {
                  pending->Packets.push_back (std::move (packet));
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

    void RequestJson (const FName &PacketName, const FString &JsonPayload, float TimeoutSeconds)
    {
        RequestJson (PacketName, JsonPayload, TimeoutSeconds, FZLinkStreamSendOptions{});
    }

    void RequestJson (const FName &PacketName,
                      const FString &JsonPayload,
                      float TimeoutSeconds,
                      const FZLinkStreamSendOptions &Options)
    {
        auto request = Connector.request (MakePacket (PacketName, JsonPayload, Options));
        request.timeout (
          std::chrono::milliseconds (std::max (1, static_cast<int> (TimeoutSeconds * 1000.0f))));
        if (Options.bCompress) {
            request.compress ();
        }
        auto pending = Pending;
#if __has_include("CoreMinimal.h")
        const auto request_name = to_utf8 (PacketName.ToString ());
#else
        const auto request_name = to_utf8 (PacketName);
#endif
        request.submit<zlink::stream_connector::packet_t> (
          [pending, request_name] (
            zlink::stream_connector::result_t<zlink::stream_connector::packet_t> result) {
              if (!result) {
                  return;
              }
              FZLinkStreamPacket packet = ToUnrealPacket (result.value ());
#if __has_include("CoreMinimal.h")
              packet.PacketName = FName (UTF8_TO_TCHAR (request_name.c_str ()));
#else
              packet.PacketName = request_name;
#endif
              std::lock_guard<std::mutex> lock (pending->Mutex);
              if (pending->CancelCallbacks) {
                  return;
              }
              pending->Requests.push_back (std::move (packet));
          });
    }

    void Dispatch ()
    {
        Connector.dispatch ();
        std::vector<EZLinkStreamConnectionState> pending_states;
        std::vector<FZLinkStreamPacket> pending_packets;
        std::vector<FZLinkStreamPacket> pending_requests;
        std::vector<std::pair<std::uint64_t, FZLinkStreamReplyReceivedContext>> pending_replies;
        bool cancel_callbacks = false;
        UZLinkStreamConnector *owner = nullptr;
        {
            std::lock_guard<std::mutex> lock (Pending->Mutex);
            owner = Owner ();
            cancel_callbacks = Pending->CancelCallbacks;
            pending_states.swap (Pending->States);
            pending_replies.swap (Pending->Replies);
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
        if (!cancel_callbacks) {
            for (auto &packet : pending_packets) {
                owner->OnPacketReceived.Broadcast (packet);
                owner->OnPacketReceivedNative.Broadcast (packet);
            }
            for (auto &packet : pending_requests) {
                owner->OnRequestCompleted.Broadcast (packet);
                owner->OnRequestCompletedNative.Broadcast (packet);
            }
        }
        for (const auto &[id, context] : pending_replies) {
            std::shared_ptr<TFunction<void (const FZLinkStreamReplyReceivedContext &)>> callback;
            {
                std::lock_guard<std::mutex> lock (Hooks->Mutex);
                const auto entry = Hooks->Reply.find (id);
                if (entry != Hooks->Reply.end ()) {
                    callback = entry->second.Callback;
                }
            }
            if (callback) {
                (*callback) (context);
            }
        }
    }

    bool IsConnected () const { return Connector.is_connected (); }

    int PendingDispatchCount () const
    {
        std::size_t local_count = 0;
        {
            std::lock_guard<std::mutex> lock (Pending->Mutex);
            local_count = Pending->States.size () + Pending->Packets.size ()
                          + Pending->Requests.size () + Pending->Replies.size ();
        }
        return static_cast<int> (local_count + Connector.pending_dispatch_count ());
    }

    EZLinkStreamConnectionState LastState () const
    {
        std::lock_guard<std::mutex> lock (Pending->Mutex);
        return Pending->LastConnectionState;
    }

    FZLinkStreamSubscriptionHandle
    OnRequestSending (TFunction<void (FZLinkStreamRequestSendingContext &)> Callback)
    {
        std::lock_guard<std::mutex> lock (Hooks->Mutex);
        const auto id = Hooks->NextId++;
        auto wrapped = std::make_shared<TFunction<void (FZLinkStreamRequestSendingContext &)>> (
          std::move (Callback));
        Hooks->Sending.emplace (id, sending_entry_t{wrapped, RegisterSending (wrapped)});
        return MakeHandle (id, true);
    }

    FZLinkStreamSubscriptionHandle
    OnReplyReceived (TFunction<void (const FZLinkStreamReplyReceivedContext &)> Callback)
    {
        std::lock_guard<std::mutex> lock (Hooks->Mutex);
        const auto id = Hooks->NextId++;
        auto wrapped =
          std::make_shared<TFunction<void (const FZLinkStreamReplyReceivedContext &)>> (
            std::move (Callback));
        Hooks->Reply.emplace (id, reply_entry_t{wrapped, RegisterReply (id)});
        return MakeHandle (id, false);
    }

  private:
    struct sending_entry_t
    {
        std::shared_ptr<TFunction<void (FZLinkStreamRequestSendingContext &)>> Callback;
        zlink::stream_connector::subscription_t Subscription;
    };
    struct reply_entry_t
    {
        std::shared_ptr<TFunction<void (const FZLinkStreamReplyReceivedContext &)>> Callback;
        zlink::stream_connector::subscription_t Subscription;
    };
    struct hook_state_t
    {
        std::mutex Mutex;
        std::uint64_t NextId = 1;
        std::map<std::uint64_t, sending_entry_t> Sending;
        std::map<std::uint64_t, reply_entry_t> Reply;
    };

    FZLinkStreamSubscriptionHandle MakeHandle (std::uint64_t id, bool sending)
    {
        std::weak_ptr<hook_state_t> hooks = Hooks;
        return FZLinkStreamSubscriptionHandle ([hooks, id, sending] {
            if (auto state = hooks.lock ()) {
                std::lock_guard<std::mutex> lock (state->Mutex);
                if (sending) {
                    state->Sending.erase (id);
                } else {
                    state->Reply.erase (id);
                }
            }
        });
    }

    zlink::stream_connector::subscription_t RegisterSending (
      const std::shared_ptr<TFunction<void (FZLinkStreamRequestSendingContext &)>> &callback)
    {
        return Connector.on_request_sending (
          [callback] (zlink::stream_connector::request_sending_context_t &source) {
              FZLinkStreamRequestSendingContext context;
              context.RequestPacketName = to_name (source.request_packet_name);
              context.bHasActorId = source.actor_id.has_value ();
              if (source.actor_id) {
                  context.ActorId = from_utf8 (*source.actor_id);
              }
              (*callback) (context);
#if __has_include("CoreMinimal.h")
              for (const auto &entry : context.Metadata) {
                  source.set_metadata (to_utf8 (entry.Key), to_utf8 (entry.Value));
              }
#else
              for (const auto &entry : context.Metadata) {
                  source.set_metadata (entry.first, entry.second);
              }
#endif
          });
    }

    zlink::stream_connector::subscription_t RegisterReply (std::uint64_t id)
    {
        auto pending = Pending;
        return Connector.on_reply_received (
          [pending, id] (const zlink::stream_connector::reply_received_context_t &source) {
              FZLinkStreamReplyReceivedContext context;
              context.RequestPacketName = to_name (source.request_packet_name);
              context.bHasActorId = source.actor_id.has_value ();
              if (source.actor_id) {
                  context.ActorId = from_utf8 (*source.actor_id);
              }
              context.bSucceeded = source.succeeded;
              context.bHasReply = source.reply.has_value ();
              if (source.reply) {
                  context.Reply = ToUnrealPacket (*source.reply);
              }
              context.bHasError = source.error.has_value ();
              if (source.error) {
                  context.ErrorCode = static_cast<EZLinkStreamErrorCode> (source.error->code);
                  context.ErrorMessage = from_utf8 (source.error->message);
              }
              context.ElapsedMilliseconds = source.elapsed.count ();
              std::lock_guard<std::mutex> lock (pending->Mutex);
#if __has_include("CoreMinimal.h")
              if (pending->Owner.IsValid ()) {
#else
              if (pending->Owner) {
#endif
                  pending->Replies.emplace_back (id, std::move (context));
              }
          });
    }

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
        converted.Payload.assign (payload.begin (), payload.end ());
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
        std::vector<FZLinkStreamPacket> Packets;
        std::vector<FZLinkStreamPacket> Requests;
        std::vector<std::pair<std::uint64_t, FZLinkStreamReplyReceivedContext>> Replies;
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
    std::vector<std::pair<std::string, zlink::stream_connector::subscription_t>>
      PacketSubscriptions;
    std::shared_ptr<pending_state_t> Pending;
    std::shared_ptr<hook_state_t> Hooks = std::make_shared<hook_state_t> ();
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

void UZLinkStreamConnector::Subscribe (FName PacketName)
{
    _runtime->Subscribe (PacketName);
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
                                         float TimeoutSeconds)
{
    _runtime->RequestJson (PacketName, JsonPayload, TimeoutSeconds);
}

void UZLinkStreamConnector::RequestJsonWithOptions (FName PacketName,
                                                    const FString &JsonPayload,
                                                    float TimeoutSeconds,
                                                    const FZLinkStreamSendOptions &Options)
{
    _runtime->RequestJson (PacketName, JsonPayload, TimeoutSeconds, Options);
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

FZLinkStreamSubscriptionHandle
UZLinkStreamConnector::OnRequestSending (FZLinkStreamRequestSendingDelegate Delegate)
{
    return OnRequestSending (
      [Delegate = std::move (Delegate)] (FZLinkStreamRequestSendingContext &Context) {
          Delegate.ExecuteIfBound (Context);
      });
}

FZLinkStreamSubscriptionHandle UZLinkStreamConnector::OnRequestSending (
  TFunction<void (FZLinkStreamRequestSendingContext &)> Callback)
{
    return _runtime->OnRequestSending (std::move (Callback));
}

FZLinkStreamSubscriptionHandle
UZLinkStreamConnector::OnReplyReceived (FZLinkStreamReplyReceivedDelegate Delegate)
{
    return OnReplyReceived (
      [Delegate = std::move (Delegate)] (const FZLinkStreamReplyReceivedContext &Context) {
          Delegate.ExecuteIfBound (Context);
      });
}

FZLinkStreamSubscriptionHandle UZLinkStreamConnector::OnReplyReceived (
  TFunction<void (const FZLinkStreamReplyReceivedContext &)> Callback)
{
    return _runtime->OnReplyReceived (std::move (Callback));
}
