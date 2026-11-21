# Java etcd client dependency inventory

Resolved compile/runtime and protoc/gRPC generator artifacts are pinned in [gradle.lockfile](gradle.lockfile). Test-only and adapter-only libraries are also locked there; they are not shipped as client runtime dependencies. The protobuf Gradle plugin is pinned to 0.9.4 in [build.gradle.kts](build.gradle.kts).

License names below come from the artifact POM or its parent POM. These names are an inventory, not replacement license texts. The source POM links identify the declarations.

| Artifact | Scope | Declared license | POM |
|---|---|---|---|
| `com.google.android:annotations:4.1.1.4` | runtimeClasspath | Apache 2.0 | [POM](https://repo.maven.apache.org/maven2/com/google/android/annotations/4.1.1.4/annotations-4.1.1.4.pom) |
| `com.google.api.grpc:proto-google-common-protos:2.48.0` | compileClasspath, runtimeClasspath | Apache-2.0 | [POM](https://repo.maven.apache.org/maven2/com/google/api/grpc/proto-google-common-protos/2.48.0/proto-google-common-protos-2.48.0.pom) |
| `com.google.code.findbugs:jsr305:3.0.2` | compileClasspath, runtimeClasspath | The Apache Software License, Version 2.0 | [POM](https://repo.maven.apache.org/maven2/com/google/code/findbugs/jsr305/3.0.2/jsr305-3.0.2.pom) |
| `com.google.code.gson:gson:2.11.0` | runtimeClasspath | Apache-2.0 | [POM](https://repo.maven.apache.org/maven2/com/google/code/gson/gson/2.11.0/gson-2.11.0.pom) |
| `com.google.errorprone:error_prone_annotations:2.30.0` | compileClasspath, runtimeClasspath | Apache 2.0 | [POM](https://repo.maven.apache.org/maven2/com/google/errorprone/error_prone_annotations/2.30.0/error_prone_annotations-2.30.0.pom) |
| `com.google.guava:failureaccess:1.0.2` | compileClasspath, runtimeClasspath | The Apache Software License, Version 2.0 | [POM](https://repo.maven.apache.org/maven2/com/google/guava/failureaccess/1.0.2/failureaccess-1.0.2.pom) |
| `com.google.guava:guava:33.3.1-android` | compileClasspath, runtimeClasspath | Apache License, Version 2.0 | [POM](https://repo.maven.apache.org/maven2/com/google/guava/guava/33.3.1-android/guava-33.3.1-android.pom) |
| `com.google.guava:listenablefuture:9999.0-empty-to-avoid-conflict-with-guava` | compileClasspath, runtimeClasspath | The Apache Software License, Version 2.0 | [POM](https://repo.maven.apache.org/maven2/com/google/guava/listenablefuture/9999.0-empty-to-avoid-conflict-with-guava/listenablefuture-9999.0-empty-to-avoid-conflict-with-guava.pom) |
| `com.google.j2objc:j2objc-annotations:3.0.0` | compileClasspath, runtimeClasspath | Apache License, Version 2.0 | [POM](https://repo.maven.apache.org/maven2/com/google/j2objc/j2objc-annotations/3.0.0/j2objc-annotations-3.0.0.pom) |
| `com.google.protobuf:protobuf-gradle-plugin:0.9.4` | build plugin | BSD 3-Clause | [POM](https://repo.maven.apache.org/maven2/com/google/protobuf/protobuf-gradle-plugin/0.9.4/protobuf-gradle-plugin-0.9.4.pom) |
| `com.google.protobuf:protobuf-java:4.30.2` | compileClasspath, runtimeClasspath | BSD-3-Clause | [POM](https://repo.maven.apache.org/maven2/com/google/protobuf/protobuf-java/4.30.2/protobuf-java-4.30.2.pom) |
| `com.google.protobuf:protoc:4.30.2` | protobufToolsLocator_protoc | BSD-3-Clause | [POM](https://repo.maven.apache.org/maven2/com/google/protobuf/protoc/4.30.2/protoc-4.30.2.pom) |
| `io.grpc:grpc-api:1.70.0` | compileClasspath, runtimeClasspath | Apache 2.0 | [POM](https://repo.maven.apache.org/maven2/io/grpc/grpc-api/1.70.0/grpc-api-1.70.0.pom) |
| `io.grpc:grpc-context:1.70.0` | runtimeClasspath | Apache 2.0 | [POM](https://repo.maven.apache.org/maven2/io/grpc/grpc-context/1.70.0/grpc-context-1.70.0.pom) |
| `io.grpc:grpc-core:1.70.0` | compileClasspath, runtimeClasspath | Apache 2.0 | [POM](https://repo.maven.apache.org/maven2/io/grpc/grpc-core/1.70.0/grpc-core-1.70.0.pom) |
| `io.grpc:grpc-netty-shaded:1.70.0` | compileClasspath, runtimeClasspath | Apache 2.0 | [POM](https://repo.maven.apache.org/maven2/io/grpc/grpc-netty-shaded/1.70.0/grpc-netty-shaded-1.70.0.pom) |
| `io.grpc:grpc-protobuf:1.70.0` | compileClasspath, runtimeClasspath | Apache 2.0 | [POM](https://repo.maven.apache.org/maven2/io/grpc/grpc-protobuf/1.70.0/grpc-protobuf-1.70.0.pom) |
| `io.grpc:grpc-protobuf-lite:1.70.0` | runtimeClasspath | Apache 2.0 | [POM](https://repo.maven.apache.org/maven2/io/grpc/grpc-protobuf-lite/1.70.0/grpc-protobuf-lite-1.70.0.pom) |
| `io.grpc:grpc-stub:1.70.0` | compileClasspath, runtimeClasspath | Apache 2.0 | [POM](https://repo.maven.apache.org/maven2/io/grpc/grpc-stub/1.70.0/grpc-stub-1.70.0.pom) |
| `io.grpc:grpc-util:1.70.0` | runtimeClasspath | Apache 2.0 | [POM](https://repo.maven.apache.org/maven2/io/grpc/grpc-util/1.70.0/grpc-util-1.70.0.pom) |
| `io.grpc:protoc-gen-grpc-java:1.70.0` | protobufToolsLocator_grpc | Apache 2.0 | [POM](https://repo.maven.apache.org/maven2/io/grpc/protoc-gen-grpc-java/1.70.0/protoc-gen-grpc-java-1.70.0.pom) |
| `io.perfmark:perfmark-api:0.27.0` | runtimeClasspath | Apache 2.0 | [POM](https://repo.maven.apache.org/maven2/io/perfmark/perfmark-api/0.27.0/perfmark-api-0.27.0.pom) |
| `javax.annotation:javax.annotation-api:1.3.2` | compileClasspath | CDDL + GPLv2 with classpath exception | [POM](https://repo.maven.apache.org/maven2/javax/annotation/javax.annotation-api/1.3.2/javax.annotation-api-1.3.2.pom) |
| `org.checkerframework:checker-qual:3.43.0` | compileClasspath, runtimeClasspath | The MIT License | [POM](https://repo.maven.apache.org/maven2/org/checkerframework/checker-qual/3.43.0/checker-qual-3.43.0.pom) |
| `org.codehaus.mojo:animal-sniffer-annotations:1.24` | compileClasspath, runtimeClasspath | MIT license | [POM](https://repo.maven.apache.org/maven2/org/codehaus/mojo/animal-sniffer-annotations/1.24/animal-sniffer-annotations-1.24.pom) |

`grpc-netty-shaded:1.70.0` embeds native and shaded dependencies. Its JAR retains `META-INF/LICENSE.txt`, `META-INF/NOTICE.txt`, and `META-INF/license/` texts for Tomcat Native and Maven Wrapper (Apache-2.0), AIX/NetBSD code (OpenSSL), and BoringSSL (ISC, OpenSSL/SSLeay and MIT portions). These embedded notices must remain with redistributed binaries; the Maven runtime graph alone does not enumerate shaded components.

Official etcd v3.6.16 proto and import licenses remain in [the shared protocol license directory](../../../runtime/protocol/etcd/v3.6.16/licenses/) and are packaged by `processResources` and `sourcesJar`.

From `framework/languages/java`, regenerate the lock after an intentional dependency update with:

```sh
./gradlew --no-daemon --max-workers=4 :zlink-framework-locations-etcd:dependencies --write-locks
```
