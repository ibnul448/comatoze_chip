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
        out = si.man + "\n" + si.model + "\n" + (si.full ? "1" : "0") + "\n" + si.hw + "\n" + si.board + "\n" + si.socman + "\n" + si.socmodel;
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
        LOGI("spoof %s -> %s %s%s%s%s", proc_.c_str(), man_.c_str(), model_.c_str(), full_ ? " (lengkap)" : "", socmodel_.empty() ? "" : " chip ", socmodel_.c_str());
    }

private:
    void unload() { api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY); }

    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool apply_ = false;
    bool full_ = false;
    std::string man_, model_, proc_, hw_, board_, socman_, socmodel_;
};

REGISTER_ZYGISK_MODULE(ComatozeZ)
REGISTER_ZYGISK_COMPANION(companion_handler)
