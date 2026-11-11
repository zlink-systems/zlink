# Location

!!! info "이 장을 읽고 나면"

    id만 가지고 그 Spot·Actor가 지금 어느 node에 있는지 물어볼 수 있다.
    이 장의 코드는 [예제 저장소의 tutorial README](https://github.com/zlink-systems/zlink-<언어>-examples/blob/main/tutorial/README.ko.md)에서 가져왔으며, README의 「내려받기와 설치」·「빌드」·「실행」 절을 따르면 아래 결과를 재현할 수 있다.

[Spot](21-spot.ko.md)과 [Actor](22-actor.ko.md)는 id로만 호출했다. 그것이 지금 어느 node에
있는지는 Framework가 찾는다. 그 기록을 보관하는 곳이 **Location Store**다. 이 장은
application이 그 기록을 직접 읽는 방법을 다루며, Store가 무엇을 적어 두고 process들이 그것으로
어떻게 서로를 찾는지는
[Channel 동작 원리](30-channel-patterns.ko.md#61-location-store--누가-어디-있는지-적어-두는-곳)가
다룬다.

## 1. 조회와 호출의 차이 — 조회는 대상의 상태를 바꾸지 않는다

호출은 대상에게 동작을 실행하게 한다. 조회는 **대상의 상태를 바꾸지 않고 위치만 읽는다.** 운영 화면에
"이 방이 지금 어느 서버에 있는가"를 표시할 때, 또는 대상을 옮긴 뒤 기록이 실제로 바뀌었는지
확인할 때 사용한다.

<iframe class="zlink-diagram" src="/common/diagrams/25-location-find.html" title="find는 Store에 묻고 끝난다" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/25-location-find.html" target="_blank">↗ 크게 보기</a></p>

돌아오는 값은 그 시점의 기록이다. 대상이 실제로 응답하는지까지 확인하지 않으므로, **살아
있는지를 알아야 하면 조회가 아니라 요청을 보낸다.**

## 2. 조회하는 node의 Store 등록 요건

조회는 Store를 읽는 일이므로, 조회하는 process가 그 Store를 **같은 prefix로** 등록하고 있어야
한다. 등록 코드는 [Store plugin 설정](#store-setup)에 있다.

Spot도 Actor도 실행하지 않고 호출만 하는 node라면 Location Store 하나로 충분하다. Relocation
Store는 factory를 등록한 node에만 요구된다.

### 2.1 Store plugin 설정 {#store-setup}

[두 plugin 비교](../../../common/guide/server/store-plugins.ko.md)에서 **Redis** 또는 **Redis with etcd**를 선택한다. 두 Store는 기존 API로 각각 한 번 등록한다.

아래 코드는 server의 Framework 설정 callback 안에서 실행한다. `using`·`import`·`#include`는 파일 맨 위에 둔다. 각 process는 같은 Location prefix 또는 etcd namespace를 공유하고, Redis의 두 Store는 서로 다른 key prefix를 사용한다.

#### Redis

운영 환경에서는 [성공한 변경이 되돌아가지 않는 Redis 내구성 조건](../../../common/spec/server/05-location-relocation/02-location-store-redis.ko.md#1-범위와-독자)을 충족해야 한다. 두 Store를 같은 Redis server에 둔다.

=== "C#/.NET"

    ```sh
    dotnet add package Zlink.Framework.Locations.Redis
    ```

    ```csharp
    using Zlink.Framework.Locations.Redis;

    options.AddLocationStore(new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions
    {
        ConnectionString = "127.0.0.1:6379",
        KeyPrefix = "game:location:"
    }));
    options.AddRelocationStore(new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions
    {
        ConnectionString = "127.0.0.1:6379",
        KeyPrefix = "game:relocation:"
    }));
    ```

=== "C++"

    `framework-locations-redis` extension을 설치한 `zlink_framework` package에서 가져온다.

    ```cmake
    find_package(zlink_framework CONFIG REQUIRED)
    target_link_libraries(my_server PRIVATE zlink::framework zlink::framework_locations_redis)
    ```

    ```cpp
    #include <zlink/locations/redis.hpp>
    namespace fw = zlink::framework;

    options.add_location_store<fw::redis::redis_location_store_t> ()
      .set_connection_string ("127.0.0.1:6379")
      .set_key_prefix ("game:location:");
    options.add_relocation_store<fw::redis::redis_relocation_store_t> ()
      .set_connection_string ("127.0.0.1:6379")
      .set_key_prefix ("game:relocation:");
    ```

=== "Java"

    Framework core와 같은 version을 사용한다. Kotlin도 같은 Java package를 설치한다.

    ```kotlin
    implementation("systems.zlink:zlink-framework-locations-redis:$zlinkFrameworkVersion")
    ```

    ```java
    import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
    import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
    import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions;
    import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore;

    options.addLocationStore(new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
        .setConnectionString("127.0.0.1:6379").setKeyPrefix("game:location:")));
    options.addRelocationStore(new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions()
        .setConnectionString("127.0.0.1:6379").setKeyPrefix("game:relocation:")));
    ```

=== "Kotlin"

    Framework core와 같은 version을 사용한다. Kotlin도 같은 Java package를 설치한다.

    ```kotlin
    implementation("systems.zlink:zlink-framework-locations-redis:$zlinkFrameworkVersion")
    ```

    ```kotlin
    import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
    import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
    import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions
    import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore

    options.addLocationStore(ZLinkRedisLocationStore(ZLinkRedisLocationOptions()
        .setConnectionString("127.0.0.1:6379").setKeyPrefix("game:location:")))
    options.addRelocationStore(ZLinkRedisRelocationStore(ZLinkRedisRelocationOptions()
        .setConnectionString("127.0.0.1:6379").setKeyPrefix("game:relocation:")))
    ```

=== "Node/TypeScript"

    ```sh
    npm install @zlink-systems/framework-locations-redis
    ```

    ```typescript
    import { ZLinkRedisLocationStore, ZLinkRedisRelocationStore }
      from '@zlink-systems/framework-locations-redis';

    builder.addLocationStore(new ZLinkRedisLocationStore({
      url: 'redis://127.0.0.1:6379', keyPrefix: 'game:location:'
    }));
    builder.addRelocationStore(new ZLinkRedisRelocationStore({
      url: 'redis://127.0.0.1:6379', keyPrefix: 'game:relocation:'
    }));
    ```

등록 뒤 Store 수명은 Framework가 관리한다.

#### Redis with etcd

자동 장애 전환이 필요한 배포에서 선택한다. Location Store는 서로 다른 물리 host의 etcd voter 3대에 두고, Relocation Store는 Redis에 둔다. Redis package도 설치한다. 사용 전에 namespace를 초기화하고 Store 관리 process를 운영한다. [etcd 운영 절차](../../../common/spec/server/05-location-relocation/07-location-store-etcd.ko.md)를 따른다. 아래 HTTP endpoint는 신뢰할 수 있는 개발 network용이며, 운영 환경의 TLS·인증은 같은 문서의 option으로 설정한다.

=== "C#/.NET"

    ```sh
    dotnet add package Zlink.Framework.Locations.Redis
    dotnet add package Zlink.Framework.Locations.Etcd
    ```

    ```csharp
    using Zlink.Framework.Locations.Etcd;
    using Zlink.Framework.Locations.Redis;

    options.AddLocationStore(new ZLinkEtcdLocationStore(o =>
    {
        o.Endpoints.Add("http://etcd-1:2379");
        o.Endpoints.Add("http://etcd-2:2379");
        o.Endpoints.Add("http://etcd-3:2379");
        o.Namespace = "game";
    }));
    options.AddRelocationStore(new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions
    {
        ConnectionString = "127.0.0.1:6379",
        KeyPrefix = "game:relocation:"
    }));
    ```

=== "C++"

    `framework-locations-redis` extension을 설치한 `zlink_framework` package에서 가져온다. `framework-locations-etcd`도 설치한다. Framework를 빌드할 때 `-DZLINK_FRAMEWORK_CPP_WITH_ETCD=ON`으로 gRPC 의존성을 포함한다.

    ```cmake
    find_package(zlink_framework CONFIG REQUIRED)
    target_link_libraries(my_server PRIVATE zlink::framework zlink::framework_locations_redis zlink::framework_locations_etcd)
    ```

    ```cpp
    #include <zlink/locations/etcd.hpp>
    #include <zlink/locations/redis.hpp>
    namespace fw = zlink::framework;

    fw::etcd::etcd_location_options_t location;
    location.endpoints = {"http://etcd-1:2379", "http://etcd-2:2379", "http://etcd-3:2379"};
    location.key_namespace = "game";
    options.add_location_store(
      std::make_shared<fw::etcd::etcd_location_store_t>(location));
    options.add_relocation_store<fw::redis::redis_relocation_store_t> ()
      .set_connection_string ("127.0.0.1:6379")
      .set_key_prefix ("game:relocation:");
    ```

=== "Java"

    Framework core와 같은 version을 사용한다. Kotlin도 같은 Java package를 설치한다.

    ```kotlin
    implementation("systems.zlink:zlink-framework-locations-redis:$zlinkFrameworkVersion")
    implementation("systems.zlink:zlink-framework-locations-etcd:$zlinkFrameworkVersion")
    ```

    ```java
    import java.util.List;
    import systems.zlink.framework.locations.etcd.ZLinkEtcdLocationOptions;
    import systems.zlink.framework.locations.etcd.ZLinkEtcdLocationStore;
    import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions;
    import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore;

    options.addLocationStore(new ZLinkEtcdLocationStore(new ZLinkEtcdLocationOptions()
        .setEndpoints(List.of("http://etcd-1:2379", "http://etcd-2:2379", "http://etcd-3:2379"))
        .setNamespace("game")));
    options.addRelocationStore(new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions()
        .setConnectionString("127.0.0.1:6379").setKeyPrefix("game:relocation:")));
    ```

=== "Kotlin"

    Framework core와 같은 version을 사용한다. Kotlin도 같은 Java package를 설치한다.

    ```kotlin
    implementation("systems.zlink:zlink-framework-locations-redis:$zlinkFrameworkVersion")
    implementation("systems.zlink:zlink-framework-locations-etcd:$zlinkFrameworkVersion")
    ```

    ```kotlin
    import systems.zlink.framework.locations.etcd.ZLinkEtcdLocationOptions
    import systems.zlink.framework.locations.etcd.ZLinkEtcdLocationStore
    import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions
    import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore

    options.addLocationStore(ZLinkEtcdLocationStore(ZLinkEtcdLocationOptions()
        .setEndpoints(listOf("http://etcd-1:2379", "http://etcd-2:2379", "http://etcd-3:2379"))
        .setNamespace("game")))
    options.addRelocationStore(ZLinkRedisRelocationStore(ZLinkRedisRelocationOptions()
        .setConnectionString("127.0.0.1:6379").setKeyPrefix("game:relocation:")))
    ```

=== "Node/TypeScript"

    ```sh
    npm install @zlink-systems/framework-locations-redis @zlink-systems/framework-locations-etcd
    ```

    ```typescript
    import { ZLinkEtcdLocationStore } from '@zlink-systems/framework-locations-etcd';
    import { ZLinkRedisRelocationStore } from '@zlink-systems/framework-locations-redis';

    builder.addLocationStore(new ZLinkEtcdLocationStore({
      endpoints: ['http://etcd-1:2379', 'http://etcd-2:2379', 'http://etcd-3:2379'],
      namespace: 'game'
    }));
    builder.addRelocationStore(new ZLinkRedisRelocationStore({
      url: 'redis://127.0.0.1:6379', keyPrefix: 'game:relocation:'
    }));
    ```

등록 뒤 Store 수명은 Framework가 관리한다.

## 3. id로 위치 조회

Spot은 spot manager에, Actor는 actor manager에 조회한다. 둘 다 id 하나만 받으며, 어느 node에
물을지는 정하지 않는다.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/Client/Program.cs:location-find"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/Client/main.cpp:location-find"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/Client/src/main/java/systems/zlink/tutorial/client/ClientApplication.java:location-find"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/Client/src/main/kotlin/systems/zlink/tutorial/client/LocationEndpoints.kt:location-find"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/Client/main.ts:location-find"
    ```

돌려주는 것은 id와 node 이름을 담은 참조 하나다. 찾지 못하면 빈 값을 돌려준다. 조회는 **지금
메시지를 받을 수 있는 대상만** 답하므로, 아직 만들어지는 중이거나 옮겨 가는 중인 대상도
빈 값으로 온다. 그래서 빈 값 하나를 "그런 id는 없다"로 단정하지 않는다.

## 4. 실행 결과

tutorial README의 「실행」 절을 따라 띄운 상태에서 Server와 Client를 실행한 상태에서 Client의 HTTP 표면에 아래 `curl` 요청을 보내면, 각 HTTP 응답은 `curl` stdout에 나오고 생성·조회 handler 기록은 Server process의 stdout 또는 `server.log`에 나온다.

```bash
curl -X POST http://127.0.0.1:5080/rooms \
  -H 'Content-Type: application/json' -d '{"title":"lobby"}'
# "5ce8339b-ec20-42d8-a117-a74677ad9af0"

curl http://127.0.0.1:5080/locations/rooms/5ce8339b-ec20-42d8-a117-a74677ad9af0
# {"spotId":"5ce8339b-ec20-42d8-a117-a74677ad9af0","generation":1,"node":"game-server-1"}

curl -X POST http://127.0.0.1:5080/players/p7 \
  -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
# "created"

curl http://127.0.0.1:5080/locations/players/p7
# {"actorId":"p7","generation":1,"node":"game-server-1"}

curl http://127.0.0.1:5080/locations/players/ghost
# 404
```

방과 플레이어 둘 다 `game-server-1`에 있다고 답했다. 마지막 호출은 만든 적 없는 id를 조회했다. 빈 값은 404가 된다.

## 5. 조회 결과의 사용 범위 — node 이름은 호출 주소가 아니다

조회가 돌려준 node 이름은 **그 순간의 위치**이지 호출 주소가 아니다. 호출은 [Spot](21-spot.ko.md)과
[Actor](22-actor.ko.md)에서 한 그대로 id만 준다. Framework가 호출할 때마다 같은 기록을 다시 읽기
때문에, 조회한 뒤 대상이 다른 node로 옮겨 가도 호출은 옮겨 간 node로 전달된다. node 이름을 받아
[node를 직접 호출하기](20-channel-messaging.ko.md#36-node를-직접-호출하기)로 보내면 그 따라가기를
잃는다.

참조를 그대로 넘기는 자리는 따로 있다. 닫거나 지우는 호출은 **그 참조가 가리키는
[generation](22-actor.ko.md#33-참조의-generation)**만 대상으로 삼으며, 같은 id로 다시 만들어진
대상은 그대로 둔다.

## 6. 관련 문서

- id로 호출하는 상태 객체 — [Spot](21-spot.ko.md) · [Actor](22-actor.ko.md)
- Store가 무엇을 적어 두는가 — [Channel 동작 원리](30-channel-patterns.ko.md#61-location-store--누가-어디-있는지-적어-두는-곳)
- 운영 조회 — [운영과 lifecycle](12-operations.ko.md#6-location-readiness와-운영-조회)
- 이 장 코드의 실행본 — [예제 저장소의 tutorial README](https://github.com/zlink-systems/zlink-<언어>-examples/blob/main/tutorial/README.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
