plugins {
    application
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-http-client:0.3.1")
    implementation("systems.zlink:zlink-stream-connector:0.1.0-SNAPSHOT")
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

application {
    applicationName = "to-actor-client"
    mainClass.set("systems.zlink.e2e.toactormessaging.client.Program")
}
