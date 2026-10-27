plugins { application }

dependencies {
    implementation(project(":server:shared"))
    implementation(
        platform("org.springframework.boot:spring-boot-dependencies:${rootProject.extra["springBootVersion"]}")
    )
    implementation("io.netty:netty-codec-http")
}

application {
    mainClass.set("MyGame.lobby.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}

dependencies {
    testImplementation(project(":server:match"))
    testImplementation("org.junit.jupiter:junit-jupiter:${rootProject.extra["junitVersion"]}")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}

tasks.test { useJUnitPlatform() }
