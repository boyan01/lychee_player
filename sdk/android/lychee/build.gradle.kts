// The AAR tech.soit.lychee:lychee: the core as liblychee.so (also a prefab
// module, for native callers of the C ABI), without FFmpeg, and the Kotlin
// LycheePlayer, LycheeFrameDriver and LycheePlayerView over it.
//
//   ./gradlew :lychee:publish   # into build/repo, where the Flutter plugin
//                               # looks for it

plugins {
    id("com.android.library")
    `maven-publish`
}

val repoRoot = rootDir.resolve("../..")
// The same version as the Flutter plugin and the Apple frameworks.
val lycheeVersion = Regex("""(?m)^version: (\S+)""")
    .find(repoRoot.resolve("flutter/pubspec.yaml").readText())!!.groupValues[1]

// The base AAR never links FFmpeg; the optional :ffmpeg module supplies it.
val supportedAbis = listOf("armeabi-v7a", "arm64-v8a", "x86_64")

android {
    namespace = "tech.soit.lychee"
    compileSdk = 36

    defaultConfig {
        // AAudio.
        minSdk = 26
        ndk {
            abiFilters += supportedAbis
        }
        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_shared"
                arguments += "-DLYCHEE_SDK_VERSION=$lycheeVersion"
                // Always explicit: CMake's cache would keep the last value.
                arguments += "-DLYCHEE_WITH_FFMPEG=OFF"
            }
        }
        consumerProguardFiles("consumer-rules.pro")
    }

    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        prefabPublishing = true
    }

    prefab {
        create("lychee") {
            headers = repoRoot.resolve("core/include").path
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    publishing {
        singleVariant("release")
    }
}

// The SDK notices distinguish the base and optional artifacts. Keep them
// as a Java resource, so that they end up in every APK as
// META-INF/lychee/THIRD_PARTY_NOTICES.
abstract class CopyNotices : DefaultTask() {
    @get:InputFile
    abstract val notices: RegularFileProperty

    @get:OutputDirectory
    abstract val output: DirectoryProperty

    @TaskAction
    fun copy() {
        val dir = output.get().asFile.resolve("META-INF/lychee")
        dir.mkdirs()
        notices.get().asFile.copyTo(dir.resolve("THIRD_PARTY_NOTICES"), overwrite = true)
    }
}

val copyNotices = tasks.register<CopyNotices>("copyNotices") {
    notices.set(repoRoot.resolve("THIRD_PARTY_NOTICES"))
}

androidComponents {
    onVariants { variant ->
        variant.sources.resources?.addGeneratedSourceDirectory(copyNotices, CopyNotices::output)
    }
}

// The release ships the AAR and its POM (tool/release.sh); without Gradle
// module metadata, the POM alone describes the dependencies.
tasks.withType<GenerateModuleMetadata>().configureEach {
    enabled = false
}

publishing {
    publications {
        register<MavenPublication>("release") {
            groupId = "tech.soit.lychee"
            artifactId = "lychee"
            version = lycheeVersion
            afterEvaluate {
                from(components["release"])
            }
        }
    }
    repositories {
        maven {
            name = "local"
            url = uri(rootProject.layout.buildDirectory.dir("repo"))
        }
    }
}
