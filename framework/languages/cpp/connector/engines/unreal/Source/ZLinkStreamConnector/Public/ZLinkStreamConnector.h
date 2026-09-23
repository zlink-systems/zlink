/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#if __has_include("CoreMinimal.h")
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ZLinkStreamConnector.generated.h"
#else
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#define UCLASS(...)
#define UFUNCTION(...)
#define UPROPERTY(...)
#define USTRUCT(...)
#define GENERATED_BODY()
#define UENUM(...)
#define BlueprintType
#define BlueprintCallable
#define BlueprintAssignable
#define BlueprintReadOnly
#define DECLARE_DYNAMIC_DELEGATE_OneParam(Name, ParamType, ParamName)                              \
    class Name                                                                                     \
    {                                                                                              \
      public:                                                                                      \
        template <typename CallbackT> void BindLambda (CallbackT callback)                         \
        {                                                                                          \
            Function = callback;                                                                   \
        }                                                                                          \
        void ExecuteIfBound (ParamType value) const                                                \
        {                                                                                          \
            if (Function) {                                                                        \
                Function (value);                                                                  \
            }                                                                                      \
        }                                                                                          \
                                                                                                   \
      private:                                                                                     \
        std::function<void (ParamType)> Function;                                                  \
    }
#define DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(Name, ParamType, ParamName)                    \
    class Name                                                                                     \
    {                                                                                              \
      public:                                                                                      \
        void Broadcast (ParamType value)                                                           \
        {                                                                                          \
            LastValue = value;                                                                     \
            ++BroadcastCount;                                                                      \
        }                                                                                          \
        int NumBroadcasts () const noexcept                                                        \
        {                                                                                          \
            return BroadcastCount;                                                                 \
        }                                                                                          \
        ParamType LastBroadcastValue () const noexcept                                             \
        {                                                                                          \
            return LastValue;                                                                      \
        }                                                                                          \
                                                                                                   \
      private:                                                                                     \
        int BroadcastCount = 0;                                                                    \
        ParamType LastValue{};                                                                     \
    }
#define DECLARE_MULTICAST_DELEGATE_OneParam(Name, ParamType)                                       \
    class Name                                                                                     \
    {                                                                                              \
      public:                                                                                      \
        template <typename CallbackT> void AddLambda (CallbackT callback)                          \
        {                                                                                          \
            Function = callback;                                                                   \
        }                                                                                          \
        void Broadcast (ParamType value)                                                           \
        {                                                                                          \
            if (Function) {                                                                        \
                Function (value);                                                                  \
            }                                                                                      \
            ++BroadcastCount;                                                                      \
        }                                                                                          \
        int NumBroadcasts () const noexcept                                                        \
        {                                                                                          \
            return BroadcastCount;                                                                 \
        }                                                                                          \
                                                                                                   \
      private:                                                                                     \
        std::function<void (ParamType)> Function;                                                  \
        int BroadcastCount = 0;                                                                    \
    }
class UObject
{
};
using FString = std::string;
using FName = std::string;
using uint8 = std::uint8_t;
template <typename T> using TArray = std::vector<T>;
template <typename K, typename V> using TMap = std::map<K, V>;
template <typename Signature> using TFunction = std::function<Signature>;
using int32 = std::int32_t;
#endif

#if __has_include("CoreMinimal.h")
#include <memory>
#endif

UENUM (BlueprintType)
enum class EZLinkStreamConnectionState : std::uint8_t
{
    Created,
    Connecting,
    Connected,
    Reconnecting,
    Disconnected,
    Closed
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam (FZLinkStreamConnectionStateChanged,
                                             EZLinkStreamConnectionState,
                                             State);

USTRUCT (BlueprintType)
struct FZLinkStreamPacket
{
    GENERATED_BODY ()

    UPROPERTY (BlueprintReadWrite, Category = "ZLink")
    FName PacketName;

    UPROPERTY (BlueprintReadWrite, Category = "ZLink")
    TArray<uint8> Payload;

    UPROPERTY (BlueprintReadWrite, Category = "ZLink")
    TMap<FString, FString> Metadata;

    UPROPERTY (BlueprintReadWrite, Category = "ZLink")
    bool bCompressed = false;
};

USTRUCT (BlueprintType)
struct FZLinkStreamSendOptions
{
    GENERATED_BODY ()

    UPROPERTY (BlueprintReadWrite, Category = "ZLink")
    TMap<FString, FString> Metadata;

    UPROPERTY (BlueprintReadWrite, Category = "ZLink")
    bool bCompress = false;
};

USTRUCT (BlueprintType)
struct FZLinkStreamSubscriptionHandle
{
    GENERATED_BODY ()

    UPROPERTY (BlueprintReadOnly, Category = "ZLink")
    int32 Id = 0;
};

USTRUCT (BlueprintType)
struct FZLinkStreamRequestResult
{
    GENERATED_BODY ()

    UPROPERTY (BlueprintReadOnly, Category = "ZLink")
    bool bSuccess = false;

    UPROPERTY (BlueprintReadOnly, Category = "ZLink")
    FZLinkStreamPacket Packet;

    UPROPERTY (BlueprintReadOnly, Category = "ZLink")
    int32 ErrorCode = 0;

    UPROPERTY (BlueprintReadOnly, Category = "ZLink")
    FString ErrorMessage;
};

DECLARE_DYNAMIC_DELEGATE_OneParam (FZLinkStreamPacketDelegate, FZLinkStreamPacket, Packet);
DECLARE_DYNAMIC_DELEGATE_OneParam (FZLinkStreamRequestDelegate, FZLinkStreamRequestResult, Result);

namespace UE::ZLinkStreamConnector::Private
{
class FZLinkStreamConnectorRuntime;
}

UCLASS (BlueprintType)
class UZLinkStreamConnector : public UObject
{
    GENERATED_BODY ()

  public:
    UZLinkStreamConnector ();
    ~UZLinkStreamConnector ();

    UFUNCTION (BlueprintCallable, Category = "ZLink")
    void Connect (const FString &Endpoint);

    UFUNCTION (BlueprintCallable, Category = "ZLink")
    void Close ();

    UFUNCTION (BlueprintCallable, Category = "ZLink")
    FZLinkStreamSubscriptionHandle On (FName PacketName, FZLinkStreamPacketDelegate Delegate);

    FZLinkStreamSubscriptionHandle On (FName PacketName,
                                       TFunction<void (const FZLinkStreamPacket &)> Callback);

    UFUNCTION (BlueprintCallable, Category = "ZLink")
    void Unsubscribe (FZLinkStreamSubscriptionHandle Handle);

    UFUNCTION (BlueprintCallable, Category = "ZLink")
    void SendJson (FName PacketName, const FString &JsonPayload);

    UFUNCTION (BlueprintCallable, Category = "ZLink")
    void SendJsonWithOptions (FName PacketName,
                              const FString &JsonPayload,
                              const FZLinkStreamSendOptions &Options);

    UFUNCTION (BlueprintCallable, Category = "ZLink")
    void RequestJson (FName PacketName,
                      const FString &JsonPayload,
                      float TimeoutSeconds,
                      FZLinkStreamRequestDelegate OnCompleted);

    void RequestJson (FName PacketName,
                      const FString &JsonPayload,
                      float TimeoutSeconds,
                      TFunction<void (const FZLinkStreamRequestResult &)> OnCompleted);

    UFUNCTION (BlueprintCallable, Category = "ZLink")
    void RequestJsonWithOptions (FName PacketName,
                                 const FString &JsonPayload,
                                 float TimeoutSeconds,
                                 const FZLinkStreamSendOptions &Options,
                                 FZLinkStreamRequestDelegate OnCompleted);

    void RequestJsonWithOptions (FName PacketName,
                                 const FString &JsonPayload,
                                 float TimeoutSeconds,
                                 const FZLinkStreamSendOptions &Options,
                                 TFunction<void (const FZLinkStreamRequestResult &)> OnCompleted);

    UFUNCTION (BlueprintCallable, Category = "ZLink")
    void Dispatch ();

    UFUNCTION (BlueprintCallable, Category = "ZLink")
    void Tick (float DeltaSeconds);

    UFUNCTION (BlueprintCallable, Category = "ZLink")
    void ShutdownForPie ();

    UFUNCTION (BlueprintCallable, Category = "ZLink")
    void ShutdownForMapUnload ();

    UFUNCTION (BlueprintCallable, Category = "ZLink")
    void ShutdownForGameInstanceShutdown ();

    bool IsConnected () const;
    int PendingDispatchCount () const;
    EZLinkStreamConnectionState LastState () const;

    UPROPERTY (BlueprintAssignable, Category = "ZLink")
    FZLinkStreamConnectionStateChanged OnConnectionStateChanged;

  private:
    std::unique_ptr<UE::ZLinkStreamConnector::Private::FZLinkStreamConnectorRuntime> _runtime;
};
