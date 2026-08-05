plugins {
    application
    id("org.jetbrains.kotlin.jvm")
    id("org.jetbrains.kotlin.plugin.spring")
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-kotlin:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-codec-protobuf:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-codec-msgpack:0.1.0-SNAPSHOT")
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("com.google.protobuf:protobuf-java:4.30.2")
    implementation("org.springframework.boot:spring-boot-starter:3.5.14")
}

kotlin {
    jvmToolchain(22)
}

application {
    applicationName = "registration-codec-kotlin-json-only-peer"
    mainClass.set("systems.zlink.e2e.kotlin.registrationcodec.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
