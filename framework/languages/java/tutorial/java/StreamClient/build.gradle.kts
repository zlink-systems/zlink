plugins {
    application
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(25))
    }
}

dependencies {
    implementation(project(":java:Shared"))
    // This process is outside the mesh. It references the connector only, never
    // the Framework.
    implementation(libs.zlink.stream.connector)
}

application {
    mainClass.set("systems.zlink.tutorial.streamclient.StreamClientProgram")
}
