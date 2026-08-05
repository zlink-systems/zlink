plugins {
    application
}

dependencies {
    implementation(project("${path.substringBeforeLast(":Client")}:Shared"))
    implementation(project("${path.substringBeforeLast(":Client")}:Server:Configuration"))
    implementation("systems.zlink:zlink-stream-connector:0.9.0")
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(22))
    }
}

application {
    mainClass.set("systems.zlink.samples.supportchat.client.Program")
}
