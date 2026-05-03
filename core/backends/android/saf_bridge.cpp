#include "saf_bridge.h"

#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <string>

// The android_app pointer is owned by backend.cpp inside namespace
// backend. We pull it via an extern declaration so we can attach to
// the JVM and find MainActivity.
namespace backend { extern struct android_app* app; }

namespace android_saf {

namespace {
    constexpr const char* kLogTag = "saf_bridge";

    // Helper: attach current native thread to the JVM, look up the
    // requested method on MainActivity, and return everything the
    // caller needs to perform a Call*Method then detach. On failure
    // logs and returns false.
    struct JniScope {
        JavaVM*  vm     = nullptr;
        JNIEnv*  env    = nullptr;
        jclass   clazz  = nullptr;
        jobject  self   = nullptr;
        bool     attached = false;
        bool ok() const { return env != nullptr && clazz != nullptr; }

        ~JniScope() {
            if (clazz) env->DeleteLocalRef(clazz);
            if (attached) vm->DetachCurrentThread();
        }
    };

    bool open_jni(JniScope& s) {
        android_app* a = backend::app;
        if (!a || !a->activity || !a->activity->vm) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "no app/activity");
            return false;
        }
        s.vm   = a->activity->vm;
        s.self = a->activity->clazz;
        if (s.vm->GetEnv((void**)&s.env, JNI_VERSION_1_6) == JNI_EDETACHED) {
            if (s.vm->AttachCurrentThread(&s.env, nullptr) != JNI_OK) {
                __android_log_print(ANDROID_LOG_ERROR, kLogTag, "AttachCurrentThread failed");
                return false;
            }
            s.attached = true;
        }
        if (!s.env) return false;
        s.clazz = s.env->GetObjectClass(s.self);
        if (!s.clazz) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "GetObjectClass failed");
            return false;
        }
        return true;
    }

    std::string jstring_to_std(JNIEnv* env, jstring js) {
        if (!js) return std::string();
        const char* c = env->GetStringUTFChars(js, nullptr);
        std::string out = c ? std::string(c) : std::string();
        if (c) env->ReleaseStringUTFChars(js, c);
        env->DeleteLocalRef(js);
        return out;
    }
}

std::string pickFileForReadBlocking(const std::string& mimeFilter) {
    JniScope s;
    if (!open_jni(s)) return "";
    jmethodID m = s.env->GetMethodID(s.clazz, "safPickFileForRead",
                                     "(Ljava/lang/String;)Ljava/lang/String;");
    if (!m) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "no safPickFileForRead");
        return "";
    }
    jstring jmime = s.env->NewStringUTF(mimeFilter.c_str());
    jstring jres  = (jstring)s.env->CallObjectMethod(s.self, m, jmime);
    s.env->DeleteLocalRef(jmime);
    return jstring_to_std(s.env, jres);
}

std::string pickFolderBlocking() {
    JniScope s;
    if (!open_jni(s)) return "";
    jmethodID m = s.env->GetMethodID(s.clazz, "safPickFolder",
                                     "()Ljava/lang/String;");
    if (!m) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "no safPickFolder");
        return "";
    }
    jstring jres = (jstring)s.env->CallObjectMethod(s.self, m);
    return jstring_to_std(s.env, jres);
}

bool saveFileBlocking(const std::string& suggestedName,
                      const std::string& sourceCachePath) {
    JniScope s;
    if (!open_jni(s)) return false;
    jmethodID m = s.env->GetMethodID(s.clazz, "safSaveFile",
                                     "(Ljava/lang/String;Ljava/lang/String;)Z");
    if (!m) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "no safSaveFile");
        return false;
    }
    jstring jname = s.env->NewStringUTF(suggestedName.c_str());
    jstring jsrc  = s.env->NewStringUTF(sourceCachePath.c_str());
    jboolean ok   = s.env->CallBooleanMethod(s.self, m, jname, jsrc);
    s.env->DeleteLocalRef(jname);
    s.env->DeleteLocalRef(jsrc);
    return ok == JNI_TRUE;
}

} // namespace android_saf
