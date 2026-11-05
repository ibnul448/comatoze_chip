// Logika murni (tanpa header Android) supaya bisa diuji di komputer biasa.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Baris config: paket|key|manufacturer|model[|on[|full[|hardware|board|soc_manufacturer|soc_model]]]
//   on   : "0" = spoof dimatikan untuk paket ini (default 1 bila kosong / format lama)
//   full : "1" = ikut ganti Build.BRAND/DEVICE/PRODUCT (default 0)
//   4 kolom terakhir (opsional, boleh kosong): chip -> Build.HARDWARE, Build.BOARD, Build.SOC_MANUFACTURER, Build.SOC_MODEL
struct SpoofInfo {
    std::string man, model, hw, board, socman, socmodel;
    bool full = false;
};

// Cocok bila nama proses == paket, atau proses turunan "paket:nama".
static inline bool comatoze_lookup(const char *cfgpath, const std::string &proc, SpoofInfo &out) {
    FILE *f = fopen(cfgpath, "r");
    if (!f) return false;
    char *line = nullptr;
    size_t cap = 0;
    bool found = false;
    while (!found && getline(&line, &cap, f) > 0) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        std::vector<std::string> v;
        size_t start = 0;
        for (;;) {
            size_t p = l.find('|', start);
            if (p == std::string::npos) { v.push_back(l.substr(start)); break; }
            v.push_back(l.substr(start, p - start));
            start = p + 1;
        }
        if (v.size() < 4) continue;
        const std::string &pkg = v[0];
        if (pkg.empty()) continue;
        bool match = (proc == pkg) ||
                     (proc.size() > pkg.size() && proc.compare(0, pkg.size(), pkg) == 0 && proc[pkg.size()] == ':');
        if (!match) continue;
        if (v.size() > 4 && v[4] == "0") continue;  // dimatikan
        if (v[2].empty() || v[3].empty()) continue;
        out = SpoofInfo();
        out.man = v[2];
        out.model = v[3];
        out.full = (v.size() > 5 && v[5] == "1");
        if (v.size() > 6) out.hw = v[6];
        if (v.size() > 7) out.board = v[7];
        if (v.size() > 8) out.socman = v[8];
        if (v.size() > 9) out.socmodel = v[9];
        found = true;
    }
    free(line);
    fclose(f);
    return found;
}
