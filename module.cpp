// Comatoze Zygisk: spoof Build.MANUFACTURER & Build.MODEL per aplikasi.
// Alur: preAppSpecialize -> tanya companion (root) apakah proses ini ada di daftar
//       /data/adb/ultrapower/spoof_apps -> postAppSpecialize -> set field Build lewat JNI
//       (MANUFACTURER & MODEL; plus BRAND/DEVICE/PRODUCT bila "mode lengkap" aktif untuk app itu).
// Companion dipakai karena proses zygote tidak boleh membaca /data/adb langsung (SELinux).
#include <android/log.h>
#include <fcntl.h>
#include <jni.h>
#include <stdint.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "lookup.h"
#include "zygisk.hpp"

#define TAG "ComatozeZ"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static const char *kConfig = "/data/adb/ultrapower/spoof_apps";

static bool read_full(int fd, void *buf, size_t n) {
    char *p = static_cast<char *>(buf);
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}
static bool write_full(int fd, const void *buf, size_t n) {
    const char *p = static_cast<const char *>(buf);
    while (n > 0) {
        ssize_t r = write(fd, p, n);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

// Berjalan di proses companion (root).
static void companion_handler(int fd) {
    uint32_t n = 0;
    if (!read_full(fd, &n, sizeof(n)) || n == 0 || n > 256) {
        uint32_t z = 0;
        write_full(fd, &z, sizeof(z));
        return;
    }
    std::string proc(n, '\0');
    if (!read_full(fd, &proc[0], n)) return;
    SpoofInfo si;
    std::string out;
    if (access(kConfig, R_OK) != 0) LOGI("companion: config tidak terbaca (%s)", kConfig);
    if (comatoze_lookup(kConfig, proc, si))
        out = si.man + "\n" + si.model + "\n" + (si.full ? "1" : "0") + "\n" + si.hw + "\n" + si.board + "\n" + si.socman + "\n" + si.socmodel + "\n" + (si.props ? "1" : "0");
    LOGI("companion: %s -> %s", proc.c_str(), out.empty() ? "miss" : "HIT");
    uint32_t m = static_cast<uint32_t>(out.size());
    write_full(fd, &m, sizeof(m));
    if (m) write_full(fd, out.data(), m);
}

static void set_build_field(JNIEnv *env, jclass cls, const char *name, const std::string &value) {
    jfieldID f = env->GetStaticFieldID(cls, name, "Ljava/lang/String;");
    if (!f) {
        env->ExceptionClear();
        return;
    }
    jstring s = env->NewStringUTF(value.c_str());
    if (!s) {
        env->ExceptionClear();
        return;
    }
    env->SetStaticObjectField(cls, f, s);
    env->DeleteLocalRef(s);
}


// ---- sadap properti sistem versi Java: android.os.SystemProperties.native_get(String key, String def) ----
// Hanya untuk app yang diaktifkan ("Properti" di WebUI). Menutup pembacaan lewat Java/reflection; TIDAK menutup
// getprop (proses lain) maupun pembacaan native langsung (__system_property_get) oleh engine game.
static std::string g_hw, g_board, g_socman, g_socmodel;
static jstring (*orig_native_get)(JNIEnv *, jclass, jstring, jstring) = nullptr;

static bool spoof_prop(const char *k, std::string &v) {
    struct { const char *key; const std::string *val; } t[] = {
        {"ro.soc.model", &g_socmodel},   {"ro.soc.manufacturer", &g_socman},   {"ro.board.platform", &g_board},
        {"ro.product.board", &g_board},  {"ro.hardware", &g_hw},               {"ro.boot.hardware", &g_hw},
        {"ro.hardware.chipname", &g_socmodel}};
    for (auto &e : t)
        if (!strcmp(k, e.key) && !e.val->empty()) { v = *e.val; return true; }
    if (!strcmp(k, "ro.mediatek.platform") && g_socman == "Mediatek" && !g_socmodel.empty()) { v = g_socmodel; return true; }
    return false;
}

static jstring hook_native_get(JNIEnv *env, jclass clazz, jstring key, jstring def) {
    if (key) {
        const char *k = env->GetStringUTFChars(key, nullptr);
        std::string v;
        bool hit = k && spoof_prop(k, v);
        if (k) env->ReleaseStringUTFChars(key, k);
        if (hit) {
            jstring r = env->NewStringUTF(v.c_str());
            if (r) return r;
            env->ExceptionClear();
        }
    }
    return orig_native_get ? orig_native_get(env, clazz, key, def) : def;
}

class ComatozeZ : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        if (!args || !args->nice_name) return unload();
        const char *nm = env_->GetStringUTFChars(args->nice_name, nullptr);
        std::string proc = nm ? nm : "";
        if (nm) env_->ReleaseStringUTFChars(args->nice_name, nm);
        if (proc.empty() || proc.size() > 256) return unload();

        int fd = api_->connectCompanion();
        if (fd < 0) {
            LOGI("connectCompanion gagal untuk %s", proc.c_str());
            return unload();
        }
        uint32_t n = static_cast<uint32_t>(proc.size());
        uint32_t m = 0;
        std::string reply;
        bool ok = write_full(fd, &n, sizeof(n)) && write_full(fd, proc.data(), n) &&
                  read_full(fd, &m, sizeof(m)) && m > 0 && m <= 512;
        if (ok) {
            reply.resize(m);
            ok = read_full(fd, &reply[0], m);
        }
        close(fd);
        if (!ok) return unload();  // balasan kosong = proses ini tidak ada di daftar (normal)

        // balasan: man \n model \n full \n hw \n board \n socman \n socmodel (kolom chip boleh kosong)
        std::vector<std::string> parts;
        size_t st = 0;
        for (;;) {
            size_t p = reply.find('\n', st);
            if (p == std::string::npos) { parts.push_back(reply.substr(st)); break; }
            parts.push_back(reply.substr(st, p - st));
            st = p + 1;
        }
        if (parts.size() < 3) return unload();
        man_ = parts[0];
        model_ = parts[1];
        full_ = (parts[2] == "1");
        if (parts.size() >= 7) { hw_ = parts[3]; board_ = parts[4]; socman_ = parts[5]; socmodel_ = parts[6]; }
        if (parts.size() >= 8) props_ = (parts[7] == "1");
        if (man_.empty() || model_.empty()) return unload();
        apply_ = true;
        proc_ = proc;
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!apply_) return;
        jclass cls = env_->FindClass("android/os/Build");
        if (!cls) {
            env_->ExceptionClear();
            return;
        }
        set_build_field(env_, cls, "MANUFACTURER", man_);
        set_build_field(env_, cls, "MODEL", model_);
        if (full_) {  // mode lengkap: brand = manufacturer, device & product = kode model
            set_build_field(env_, cls, "BRAND", man_);
            set_build_field(env_, cls, "DEVICE", model_);
            set_build_field(env_, cls, "PRODUCT", model_);
        }
        if (!hw_.empty()) set_build_field(env_, cls, "HARDWARE", hw_);                  // chip: nama platform (qcom, mtXXXX, s5eXXXX)
        if (!board_.empty()) set_build_field(env_, cls, "BOARD", board_);
        if (!socman_.empty()) set_build_field(env_, cls, "SOC_MANUFACTURER", socman_);  // Android 12+ (field tidak ada di versi lama: dilewati)
        if (!socmodel_.empty()) set_build_field(env_, cls, "SOC_MODEL", socmodel_);
        env_->DeleteLocalRef(cls);
        bool hooked = false;
        if (props_ && (!hw_.empty() || !board_.empty() || !socman_.empty() || !socmodel_.empty())) hooked = install_prop_hook();
        LOGI("spoof %s -> %s %s%s%s%s%s", proc_.c_str(), man_.c_str(), model_.c_str(), full_ ? " (lengkap)" : "", socmodel_.empty() ? "" : " chip ", socmodel_.c_str(), props_ ? (hooked ? " +props" : " +props(GAGAL)") : "");
    }

private:
    bool install_prop_hook() {
        g_hw = hw_; g_board = board_; g_socman = socman_; g_socmodel = socmodel_;
        JNINativeMethod methods[] = {{"native_get", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", reinterpret_cast<void *>(hook_native_get)}};
        api_->hookJniNativeMethods(env_, "android/os/SystemProperties", methods, 1);
        if (env_->ExceptionCheck()) env_->ExceptionClear();
        orig_native_get = reinterpret_cast<decltype(orig_native_get)>(methods[0].fnPtr);  // diisi pointer asli oleh Zygisk
        return orig_native_get != nullptr;                                                 // null = metode tidak ditemukan: hook tidak aktif
    }
    void unload() { api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY); }

    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool apply_ = false;
    bool full_ = false;
    bool props_ = false;
    std::string man_, model_, proc_, hw_, board_, socman_, socmodel_;
};

REGISTER_ZYGISK_MODULE(ComatozeZ)
REGISTER_ZYGISK_COMPANION(companion_handler)
