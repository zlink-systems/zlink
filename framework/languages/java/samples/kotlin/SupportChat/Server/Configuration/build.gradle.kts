plugins {
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project("${path.substringBefore(":Server")}:Shared"))
    implementation("systems.zlink:zlink-framework-core:0.10.0")
    implementation("systems.zlink:zlink-framework-locations-redis:0.10.0")
    implementation("org.springframework.boot:spring-boot:3.5.14")
}

kotlin {
    jvmToolchain(22)
}
