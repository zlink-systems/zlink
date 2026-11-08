plugins {
    `java-library`
    `maven-publish`
    id("com.google.protobuf") version "0.9.4"
}

description = "ZLink Java etcd location store extension"

tasks.withType<Javadoc>().configureEach { exclude("**/internal/**") }
val etcdLicenses = copySpec {
    from(rootProject.file("../../runtime/protocol/etcd/v3.6.16/licenses")) {
        into("META-INF/licenses/etcd")
    }
}
tasks.named<ProcessResources>("processResources") { with(etcdLicenses) }
tasks.named<Jar>("sourcesJar") { with(etcdLicenses) }

dependencies {
    api(project(":zlink-framework-provider-abstractions"))
    implementation("io.grpc:grpc-netty-shaded:1.70.0")
    implementation("io.grpc:grpc-protobuf:1.70.0")
    implementation("io.grpc:grpc-stub:1.70.0")
    implementation("com.google.protobuf:protobuf-java:4.30.2")
    compileOnly("javax.annotation:javax.annotation-api:1.3.2")
    testImplementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

val prepareEtcdProto by tasks.registering {
    val source = rootProject.file("../../runtime/protocol/etcd/v3.6.16")
    val output = layout.buildDirectory.dir("etcd-proto")
    inputs.dir(source)
    outputs.dir(output)
    doLast {
        source.walkTopDown().filter { it.extension == "proto" }.forEach { file ->
            val text = file.readText()
            val pkg = Regex("package\\s+([\\w.]+)\\s*;").find(text)!!.groupValues[1]
            val target = output.get().file(file.relativeTo(source).path).asFile
            target.parentFile.mkdirs()
            target.writeText(text.replace(Regex("option java_package\\s*=\\s*\"[^\"]*\"\\s*;"), "")
                .replaceFirst(Regex("package\\s+[\\w.]+\\s*;"),
                    "package $pkg;\noption java_package = \"systems.zlink.framework.locations.etcd.internal.proto.$pkg\";"))
        }
    }
}

sourceSets.main { proto.srcDir(prepareEtcdProto) }
protobuf {
    protoc { artifact = "com.google.protobuf:protoc:4.30.2" }
    plugins { create("grpc") { artifact = "io.grpc:protoc-gen-grpc-java:1.70.0" } }
    generateProtoTasks {
        all().configureEach {
            dependsOn(prepareEtcdProto)
            plugins { create("grpc") }
        }
    }
}
