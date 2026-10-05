#include "app/android_folder.hpp"

#ifdef __ANDROID__

#include <SDL3/SDL.h>
#include <jni.h>

namespace ab::folder {

namespace {

// One call into the activity's class. Local references are dropped at the end.
struct Call {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jclass cls = nullptr;
    Call() {
        if (env == nullptr) return;
        env->PushLocalFrame(16);
        if (jobject activity = static_cast<jobject>(SDL_GetAndroidActivity())) cls = env->GetObjectClass(activity);
    }
    ~Call() {
        if (env == nullptr) return;
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->PopLocalFrame(nullptr);
    }
    jmethodID method(const char* name, const char* signature) {
        if (cls == nullptr) return nullptr;
        jmethodID id = env->GetStaticMethodID(cls, name, signature);
        if (id == nullptr) env->ExceptionClear();
        return id;
    }
    std::string text(const char* name) {
        jmethodID id = method(name, "()Ljava/lang/String;");
        if (id == nullptr) return {};
        auto s = static_cast<jstring>(env->CallStaticObjectMethod(cls, id));
        if (s == nullptr || env->ExceptionCheck()) return {};
        const char* chars = env->GetStringUTFChars(s, nullptr);
        std::string out = chars != nullptr ? chars : "";
        if (chars != nullptr) env->ReleaseStringUTFChars(s, chars);
        return out;
    }
    void run(const char* name) {
        if (jmethodID id = method(name, "()V")) env->CallStaticVoidMethod(cls, id);
    }
};

}  // namespace

bool available() {
    Call c;
    return c.method("pickFolder", "()V") != nullptr;
}

void pick() { Call().run("pickFolder"); }

Pick pickState() {
    Call c;
    jmethodID id = c.method("pickState", "()I");
    const int state = id != nullptr ? c.env->CallStaticIntMethod(c.cls, id) : 0;
    return state == 1 ? Pick::Open : state == 2 ? Pick::Chosen : state == 3 ? Pick::Cancelled : Pick::None;
}

std::string saved() { return Call().text("savedFolder"); }
std::string savedName() { return Call().text("savedFolderName"); }

void startStaging(const std::string& destination) {
    Call c;
    jmethodID id = c.method("startStaging", "(Ljava/lang/String;)V");
    if (id == nullptr) return;
    c.env->CallStaticVoidMethod(c.cls, id, c.env->NewStringUTF(destination.c_str()));
}

Staging staging() {
    Staging out;
    Call c;
    jmethodID id = c.method("stagingState", "()[I");
    if (id == nullptr) {
        out.state = Staging::State::Failed;
        out.error = "the folder chooser is not available";
        return out;
    }
    auto array = static_cast<jintArray>(c.env->CallStaticObjectMethod(c.cls, id));
    jint v[3] = {0, 0, 0};
    if (array != nullptr && !c.env->ExceptionCheck()) c.env->GetIntArrayRegion(array, 0, 3, v);
    out.state = v[0] == 1 ? Staging::State::Running : v[0] == 2 ? Staging::State::Done : v[0] == 3 ? Staging::State::Failed : Staging::State::Idle;
    out.done = v[1];
    out.total = v[2];
    out.item = c.text("stagingItem");
    out.error = c.text("stagingError");
    return out;
}

void stopStaging() { Call().run("stopStaging"); }

}  // namespace ab::folder

#else

namespace ab::folder {

bool available() { return false; }
void pick() {}
Pick pickState() { return Pick::None; }
std::string saved() { return {}; }
std::string savedName() { return {}; }
void startStaging(const std::string&) {}
Staging staging() { return {}; }
void stopStaging() {}

}  // namespace ab::folder

#endif
