// ============================================================================
// REFERENCE-ONLY sample app/build.gradle.kts
//
// This file is NOT built by the Python backend repo. It documents the
// expected gradle config for the Predator-RF Android client repo at
// github.com/JakeTheSnake0245/Predator-RF so the APK build picks up the
// right backend URL + token at compile time.
//
// Copy the relevant chunks into your real app/build.gradle.kts; do not
// drop this file in verbatim (it omits dependencies + signing config
// that are project-specific).
// ============================================================================

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.jakethesnake.predatorrf"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.jakethesnake.predatorrf"
        minSdk = 26          // Android 8.0 — covers the S22 and back to ~2017
        targetSdk = 34
        versionCode = 2      // bump on every sideload to avoid VERSION_DOWNGRADE
        versionName = "0.2.0"

        // ── Backend URL + token, read from local.properties so secrets
        //    never end up in git. local.properties (gitignored) should have:
        //
        //        predator.backend.url=http://192.168.10.5:8000
        //        predator.bearer.token=<paste from /etc/predator-rf/predator-rf.env>
        //
        val props = java.util.Properties().apply {
            val f = rootProject.file("local.properties")
            if (f.exists()) load(f.inputStream())
        }
        buildConfigField("String", "PREDATOR_BACKEND_URL",
            "\"${props.getProperty("predator.backend.url", "http://10.0.2.2:8000")}\"")
        buildConfigField("String", "PREDATOR_BEARER_TOKEN",
            "\"${props.getProperty("predator.bearer.token", "")}\"")

        // Polling cadence (seconds). Phone code reads via BuildConfig so
        // you can rebuild for a tighter / looser cadence per deployment.
        buildConfigField("int", "PREDATOR_POLL_WIFI_S",
            "${props.getProperty("predator.poll.wifi.s", "5")}")
        buildConfigField("int", "PREDATOR_POLL_CELL_S",
            "${props.getProperty("predator.poll.cell.s", "15")}")

        // CoT bulk-pull cadence; respect the docs/ANDROID_INTEGRATION.md
        // "don't go faster than every 30 s" guidance.
        buildConfigField("int", "PREDATOR_COT_PULL_S",
            "${props.getProperty("predator.cot.pull.s", "30")}")

        externalNativeBuild {
            cmake {
                // Match the decoder modules' minimum ABI list. armeabi-v7a
                // is dropped — every device new enough to run modern ATAK
                // is arm64.
                abiFilters += listOf("arm64-v8a", "x86_64")
                arguments += "-DPREDATOR_ENABLE_DSDFME=ON"
                arguments += "-DPREDATOR_ENABLE_M17=ON"
                cppFlags += "-std=c++17"
            }
        }
    }

    buildFeatures {
        buildConfig = true
        viewBinding = true
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // Sideloading: by default, build with `./gradlew assembleDebug` and
    // install the debug APK — it's signed with the standard Android debug
    // keystore (~/.android/debug.keystore) and installs cleanly on the S22
    // without any extra setup. The `release` block below is intentionally
    // UNSIGNED in this sample; running `assembleRelease` here will produce
    // an unsigned APK that adb refuses to install. Wire your own
    // `signingConfigs.release` (with a real keystore) before flipping to
    // release builds. See docs/SIDELOAD_README.md.
    buildTypes {
        getByName("debug") {
            isMinifyEnabled = false
            isDebuggable = true
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
        }
        getByName("release") {
            isMinifyEnabled = false
            // signingConfig = signingConfigs.getByName("release")  // TODO
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.12.0")
    implementation("androidx.appcompat:appcompat:1.6.1")
    implementation("com.google.android.material:material:1.11.0")
    implementation("androidx.constraintlayout:constraintlayout:2.1.4")

    // Networking — OkHttp is enough for the REST + SSE polling layer
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("com.squareup.okhttp3:logging-interceptor:4.12.0")
    implementation("com.squareup.moshi:moshi-kotlin:1.15.1")

    // Coroutines for the polling loop
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.7.3")
}
