plugins {
    application
}

dependencies {
    implementation(project(":Shared"))
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

application {
    applicationName = "to-actor-kotlin-client"
    mainClass.set("systems.zlink.e2e.kotlin.toactormessaging.client.Program")
}
