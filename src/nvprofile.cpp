#include "nvprofile.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

// Minimal NVAPI driver-settings (DRS) declarations, matching NVIDIA's MIT-licensed nvapi.h
// (github.com/NVIDIA/nvapi). Function ids are from its nvapi_interface.h.
namespace {
typedef uint16_t NvUStr[2048];
typedef void* NvDRSSessionHandle;
typedef void* NvDRSProfileHandle;
constexpr uint32_t kPreferredPstateId = 0x1057EB71;  // PREFERRED_PSTATE_ID
constexpr uint32_t kPreferMax = 1;                    // PREFERRED_PSTATE_PREFER_MAX
constexpr int kNotFoundProfile = -163, kNotFoundExe = -166, kNotFoundSetting = -160, kNeedAdmin = -137;

struct DrsProfile {
    uint32_t version;
    NvUStr profileName;
    uint32_t gpuSupport;
    uint32_t isPredefined, numOfApps, numOfSettings;
};
struct DrsApp {
    uint32_t version;
    uint32_t isPredefined;
    NvUStr appName, userFriendlyName, launcher;
};
#pragma pack(push, 4)
struct DrsBinary {
    uint32_t valueLength;
    uint8_t valueData[4096];
};
struct DrsSetting {
    uint32_t version;
    NvUStr settingName;
    uint32_t settingId;
    uint32_t settingType;  // 0 = DWORD
    uint32_t settingLocation;
    uint32_t isCurrentPredefined;
    uint32_t isPredefinedValid;
    union {
        uint32_t u32;
        DrsBinary bin;
        NvUStr str;
        uint64_t u64;
    } predefined;
    union {
        uint32_t u32;
        DrsBinary bin;
        NvUStr str;
        uint64_t u64;
    } current;
};
#pragma pack(pop)
template <class T> uint32_t Ver(int v) { return (uint32_t)(sizeof(T) | (v << 16)); }

void CopyName(NvUStr dst, const wchar_t* src) {
    memset(dst, 0, sizeof(NvUStr));
    for (int i = 0; src[i] && i < 2047; i++) dst[i] = (uint16_t)src[i];
}

typedef void*(__cdecl* QueryFn)(uint32_t);
typedef int(__cdecl* InitFn)();
typedef int(__cdecl* SessionFn)(NvDRSSessionHandle*);
typedef int(__cdecl* SessionOp)(NvDRSSessionHandle);
typedef int(__cdecl* CreateProfileFn)(NvDRSSessionHandle, DrsProfile*, NvDRSProfileHandle*);
typedef int(__cdecl* FindProfileFn)(NvDRSSessionHandle, NvUStr, NvDRSProfileHandle*);
typedef int(__cdecl* CreateAppFn)(NvDRSSessionHandle, NvDRSProfileHandle, DrsApp*);
typedef int(__cdecl* FindAppFn)(NvDRSSessionHandle, NvUStr, NvDRSProfileHandle*, DrsApp*);
typedef int(__cdecl* SetSettingFn)(NvDRSSessionHandle, NvDRSProfileHandle, DrsSetting*);
typedef int(__cdecl* GetSettingFn)(NvDRSSessionHandle, NvDRSProfileHandle, uint32_t, DrsSetting*);
typedef int(__cdecl* DeleteSettingFn)(NvDRSSessionHandle, NvDRSProfileHandle, uint32_t);
}  // namespace

bool NvSetMaxPerformance(bool on, std::string& result) {
    HMODULE lib = LoadLibraryA("nvapi64.dll");
    if (!lib) {
        result = "no NVIDIA driver";
        return false;
    }
    auto q = (QueryFn)GetProcAddress(lib, "nvapi_QueryInterface");
    if (!q) {
        result = "nvapi_QueryInterface missing";
        return false;
    }
    auto init = (InitFn)q(0x0150e828);
    auto create = (SessionFn)q(0x0694d52e);
    auto destroy = (SessionOp)q(0xdad9cff8);
    auto load = (SessionOp)q(0x375dbd6b);
    auto save = (SessionOp)q(0xfcbc7e14);
    auto createProfile = (CreateProfileFn)q(0xcc176068);
    auto findProfile = (FindProfileFn)q(0x7e4a9a0b);
    auto createApp = (CreateAppFn)q(0x4347a9de);
    auto findApp = (FindAppFn)q(0xeee566b2);
    auto setSetting = (SetSettingFn)q(0x577dd202);
    auto getSetting = (GetSettingFn)q(0x73bf8338);
    auto delSetting = (DeleteSettingFn)q(0xe4a26362);
    if (!init || !create || !destroy || !load || !save || !createProfile || !findProfile || !createApp || !findApp ||
        !setSetting || !getSetting || !delSetting) {
        result = "driver settings API missing";
        return false;
    }
    if (int e = init()) {
        result = "NvAPI_Initialize " + std::to_string(e);
        return false;
    }
    NvDRSSessionHandle s = nullptr;
    if (int e = create(&s)) {
        result = "DRS session " + std::to_string(e);
        return false;
    }
    auto fail = [&](const char* what, int e) {
        result = std::string(what) + " " + std::to_string(e) + (e == kNeedAdmin ? " (needs administrator once)" : "");
        destroy(s);
        return false;
    };
    if (int e = load(s)) return fail("load settings", e);

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* exe = wcsrchr(path, L'\\');
    exe = exe ? exe + 1 : path;

    NvUStr name;
    CopyName(name, exe);
    NvDRSProfileHandle prof = nullptr;
    DrsApp app{};
    app.version = Ver<DrsApp>(1);
    int e = findApp(s, name, &prof, &app);
    if (e == kNotFoundExe || e == kNotFoundProfile) {
        if (!on) {  // nothing to undo
            destroy(s);
            result = "not set";
            return true;
        }
        NvUStr pname;
        CopyName(pname, L"FrameLift");
        if (findProfile(s, pname, &prof) != 0) {
            DrsProfile p{};
            p.version = Ver<DrsProfile>(1);
            CopyName(p.profileName, L"FrameLift");
            if (int e2 = createProfile(s, &p, &prof)) return fail("create profile", e2);
        }
        DrsApp a{};
        a.version = Ver<DrsApp>(1);
        CopyName(a.appName, exe);
        CopyName(a.userFriendlyName, L"FrameLift");
        if (int e2 = createApp(s, prof, &a)) return fail("add application", e2);
    } else if (e) {
        return fail("find application", e);
    }

    DrsSetting cur{};
    cur.version = Ver<DrsSetting>(1);
    const int g = getSetting(s, prof, kPreferredPstateId, &cur);
    const bool isMax = g == 0 && cur.settingLocation == 0 && cur.current.u32 == kPreferMax;  // 0 = this profile
    if (on == isMax) {
        destroy(s);
        result = on ? "already on" : "already off";
        return true;
    }
    if (on) {
        DrsSetting st{};
        st.version = Ver<DrsSetting>(1);
        st.settingId = kPreferredPstateId;
        st.settingType = 0;
        st.current.u32 = kPreferMax;
        if (int e2 = setSetting(s, prof, &st)) return fail("set power mode", e2);
    } else if (int e2 = delSetting(s, prof, kPreferredPstateId); e2 && e2 != kNotFoundSetting) {
        return fail("reset power mode", e2);
    }
    if (int e2 = save(s)) return fail("save settings", e2);
    destroy(s);
    result = on ? "set" : "removed";
    return true;
}
