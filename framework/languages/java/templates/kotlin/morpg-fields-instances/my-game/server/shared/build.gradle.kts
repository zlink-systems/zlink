plugins { `java-library` }

dependencies {
    api(
        "systems.zlink:zlink-framework-spring-boot-starter:${rootProject.extra["frameworkVersion"]}"
    )
    api("systems.zlink:zlink-framework-locations-redis:${rootProject.extra["frameworkVersion"]}")
    api("org.springframework.boot:spring-boot-starter:3.5.14")
}
