// Android system properties: builds a read-only property area in bionic's legacy single-file
// layout (/dev/__properties__ as a regular file, which bionic maps as a "pre-split" area) from the
// firmware's build.prop files plus Refract's settings.
#include "properties.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

#include "common.h"
#include "vfs.h"

namespace rn {

namespace {

constexpr uint32_t kPropAreaMagic = 0x504f5250;    // "PROP"
constexpr uint32_t kPropAreaVersion = 0xfc6ed0ab;
constexpr size_t kHeaderSize = 128;                // bytes_used, serial, magic, version, reserved[28]
constexpr size_t kBtSize = 20;                     // namelen, prop, left, right, children
constexpr size_t kInfoSize = 96;                   // serial, value[PROP_VALUE_MAX]
constexpr size_t kValueMax = 92;

class AreaBuilder {
public:
    AreaBuilder() { Alloc(kBtSize + 1); }  // the root node, nameless

    void Add(const std::string& name, const std::string& value) {
        uint32_t node = 0;
        size_t start = 0;
        while (true) {
            size_t dot = name.find('.', start);
            std::string part = name.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            if (part.empty())
                return;
            node = Child(node, part);
            if (dot == std::string::npos)
                break;
            start = dot + 1;
        }
        if (Get(node + 4))
            return;  // first definition wins
        const std::string v = value.substr(0, kValueMax - 1);
        const uint32_t info = Alloc(kInfoSize + name.size() + 1);
        Set(info, static_cast<uint32_t>(v.size()) << 24);
        memcpy(&data_[info + 4], v.data(), v.size());
        memcpy(&data_[info + kInfoSize], name.data(), name.size());
        Set(node + 4, info);
    }

    std::vector<uint8_t> Finish() const {
        const size_t size = std::max<size_t>(128 * 1024, (kHeaderSize + data_.size() + 4095) & ~size_t(4095));
        std::vector<uint8_t> out(size);
        const uint32_t header[4] = {static_cast<uint32_t>(data_.size()), 0, kPropAreaMagic, kPropAreaVersion};
        memcpy(out.data(), header, sizeof(header));
        memcpy(out.data() + kHeaderSize, data_.data(), data_.size());
        return out;
    }

private:
    std::vector<uint8_t> data_;

    uint32_t Alloc(size_t n) {
        const uint32_t off = static_cast<uint32_t>(data_.size());
        data_.resize(data_.size() + ((n + 3) & ~size_t(3)));
        return off;
    }
    uint32_t Get(uint32_t off) const {
        uint32_t v;
        memcpy(&v, &data_[off], 4);
        return v;
    }
    void Set(uint32_t off, uint32_t v) { memcpy(&data_[off], &v, 4); }

    // bionic's cmp_prop_name: shorter names sort first, then strncmp.
    static int Compare(const std::string& a, const char* b, uint32_t blen) {
        if (a.size() != blen)
            return a.size() < blen ? -1 : 1;
        return strncmp(a.c_str(), b, blen);
    }

    // The child of `node` named `part` (a binary tree hangs off node->children).
    uint32_t Child(uint32_t node, const std::string& part) {
        uint32_t link = node + 16;  // children
        while (uint32_t cur = Get(link)) {
            const int c = Compare(part, reinterpret_cast<const char*>(&data_[cur + kBtSize]), Get(cur));
            if (c == 0)
                return cur;
            link = cur + (c < 0 ? 8 : 12);  // left : right
        }
        const uint32_t bt = Alloc(kBtSize + part.size() + 1);
        Set(bt, static_cast<uint32_t>(part.size()));
        memcpy(&data_[bt + kBtSize], part.data(), part.size());
        Set(link, bt);
        return bt;
    }
};

void LoadPropFile(const std::filesystem::path& path, std::map<std::string, std::string>& props) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#' || line.rfind("import ", 0) == 0)
            continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
            continue;
        props.emplace(line.substr(0, eq), line.substr(eq + 1));  // earlier files win, like init
    }
}

}  // namespace

void SetupSystemProperties(const std::wstring& sysroot, const std::wstring& data_dir,
                           const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::map<std::string, std::string> props;
    for (const auto& [k, v] : overrides)
        props[k] = v;
    // Refract's runtime, set up like scripts/launch.ps1 does for the emulator: the viewer/host bridge
    // is on this machine, and eye images go through the GPU-share layer in this process.
    props.emplace("debug.refract.host_addr", "127.0.0.1");
    props.emplace("debug.refract.gpu_share", "1");
    props.emplace("debug.refract.runtime_name", "Oculus");
    props.emplace("debug.refract.composite", "1");
    props.emplace("debug.refract.frame_sync", "0");
    props.emplace("debug.refract.hfov", "109");
    props.emplace("debug.refract.stream_scale", "100");
    props.emplace("debug.refract.stream_eyes", "2");
    props.emplace("debug.refract.direct_host", "0");
    props.emplace("debug.refract.platform.verbose", "0");
    props.emplace("ro.build.version.sdk", "32");
    props.emplace("ro.build.version.release", "12");
    props.emplace("ro.product.manufacturer", "Oculus");
    props.emplace("ro.product.model", "Quest 2");
    props.emplace("ro.product.brand", "oculus");
    props.emplace("ro.product.device", "hollywood");
    props.emplace("ro.product.name", "hollywood");
    props.emplace("ro.hardware", "hollywood");
    for (const wchar_t* f : {L"system/build.prop", L"system_ext/etc/build.prop", L"product/etc/build.prop",
                             L"vendor/build.prop", L"vendor/vendor_dlkm/etc/build.prop", L"odm/etc/build.prop",
                             L"system/system_dlkm/etc/build.prop", L"vendor/odm_dlkm/etc/build.prop"})
        LoadPropFile(std::filesystem::path(sysroot) / f, props);

    AreaBuilder area;
    for (const auto& [k, v] : props)
        area.Add(k, v);
    const std::vector<uint8_t> bytes = area.Finish();
    const std::filesystem::path out = std::filesystem::path(data_dir) / L"refract_properties";
    std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Fs().Mount("/dev/__properties__", out.wstring(), false);
    RN_INFO("system properties: %zu (debug.refract.host_addr=%s)", props.size(),
            props["debug.refract.host_addr"].c_str());
}

}  // namespace rn
