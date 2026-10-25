// The AAR tech.soit.lychee:lychee: the core as liblychee.so (also a prefab
// module, for native callers of the C ABI) with its FFmpeg, and the Kotlin
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

// FFmpeg comes from tool/build_ffmpeg.sh android-ABI; only the ABIs it was
// built for are built.
val ffmpegAbis = listOf("arm64-v8a", "x86_64")
    .filter { repoRoot.resolve("third_party/ffmpeg/android-$it").isDirectory }
if (ffmpegAbis.isEmpty()) {
    logger.warn("lychee: no Android FFmpeg in third_party/ffmpeg; run tool/build_ffmpeg.sh android-arm64-v8a")
}

android {
    namespace = "tech.soit.lychee"
    compileSdk = 36

    defaultConfig {
        // AAudio.
        minSdk = 26
        ndk {
            abiFilters += ffmpegAbis
        }
        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_shared"
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
