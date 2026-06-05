#include <windows.h>
#include <Xinput.h>

#include <cdcore/controlled_char.hpp>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace {

HMODULE g_module = nullptr;
std::string g_logPath;
std::string g_iniPath;
std::atomic<bool> g_running{true};

struct Settings {
    bool enabled = true;
    bool diagnostics = true;
    bool sharedPlayerLogging = true;
    bool writeTrace = false;
    SHORT forwardOn = 31130;
    SHORT forwardOff = 2000;
    DWORD pollRateMs = 8;
    DWORD tapDurationMs = 90;
    DWORD tapMaxDurationMs = 220;
    DWORD releaseDurationMs = 180;
    DWORD retryDelayMs = 250;
    DWORD retryWindowMs = 1200;
    DWORD triggerDelayMs = 600;
    DWORD walkingStableMs = 60;
    DWORD writeTraceMaxEvents = 200;
};

Settings g_settings;
constexpr DWORD kSharedPlayerVersion = 1;
constexpr DWORD kSharedPlayerFreshMs = 100;
constexpr DWORD kVelocityOffset = 0x1B0;
constexpr const char* kSharedPlayerMapName = "CrimsonDesert_PlayerBase_SharedMem_Bambozu";

struct SharedPlayerData {
    uintptr_t playerBase;
    DWORD lastUpdateTick;
    char hookOwnerName[64];
    DWORD version;
    uintptr_t velHookAddress;
    LONG posHookInstalled;
    uintptr_t posHookAddress;
    LONG velHookInstalled;
};

struct VelocitySnapshot {
    bool valid = false;
    uintptr_t playerBase = 0;
    float x = 0.0f;
    float z = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    DWORD ageMs = 0;
};

HANDLE g_sharedPlayerMap = nullptr;
SharedPlayerData* g_sharedPlayer = nullptr;
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_originalXInputGetState = XInputGetState;
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
XInputSetStateFn g_originalXInputSetState = XInputSetState;
using XInputGetCapabilitiesFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
XInputGetCapabilitiesFn g_originalXInputGetCapabilities = XInputGetCapabilities;
using XInputGetKeystrokeFn = DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE);
XInputGetKeystrokeFn g_originalXInputGetKeystroke = XInputGetKeystroke;
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_originalGetProcAddress = GetProcAddress;
using LoadLibraryAFn = HMODULE(WINAPI*)(LPCSTR);
LoadLibraryAFn g_originalLoadLibraryA = LoadLibraryA;
using LoadLibraryWFn = HMODULE(WINAPI*)(LPCWSTR);
LoadLibraryWFn g_originalLoadLibraryW = LoadLibraryW;
using LoadLibraryExAFn = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
LoadLibraryExAFn g_originalLoadLibraryExA = LoadLibraryExA;
using LoadLibraryExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
LoadLibraryExWFn g_originalLoadLibraryExW = LoadLibraryExW;

using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
GetRawInputDataFn g_originalGetRawInputData = GetRawInputData;
using GetRawInputDeviceInfoWFn = UINT(WINAPI*)(HANDLE, UINT, LPVOID, PUINT);
GetRawInputDeviceInfoWFn g_originalGetRawInputDeviceInfoW = GetRawInputDeviceInfoW;
using GetRawInputDeviceListFn = UINT(WINAPI*)(PRAWINPUTDEVICELIST, PUINT, UINT);
GetRawInputDeviceListFn g_originalGetRawInputDeviceList = GetRawInputDeviceList;
using RegisterRawInputDevicesFn = BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);
RegisterRawInputDevicesFn g_originalRegisterRawInputDevices = RegisterRawInputDevices;
using GetRegisteredRawInputDevicesFn = UINT(WINAPI*)(PRAWINPUTDEVICE, PUINT, UINT);
GetRegisteredRawInputDevicesFn g_originalGetRegisteredRawInputDevices = GetRegisteredRawInputDevices;

using HidD_GetAttributesFn = BOOLEAN(WINAPI*)(HANDLE, PVOID);
HidD_GetAttributesFn g_originalHidD_GetAttributes = nullptr;
using HidD_GetPreparsedDataFn = BOOLEAN(WINAPI*)(HANDLE, PVOID*);
HidD_GetPreparsedDataFn g_originalHidD_GetPreparsedData = nullptr;
using HidD_FreePreparsedDataFn = BOOLEAN(WINAPI*)(PVOID);
HidD_FreePreparsedDataFn g_originalHidD_FreePreparsedData = nullptr;
using HidD_GetFeatureFn = BOOLEAN(WINAPI*)(HANDLE, PVOID, ULONG);
HidD_GetFeatureFn g_originalHidD_GetFeature = nullptr;
using HidD_SetFeatureFn = BOOLEAN(WINAPI*)(HANDLE, PVOID, ULONG);
HidD_SetFeatureFn g_originalHidD_SetFeature = nullptr;
using HidP_GetCapsFn = LONG(WINAPI*)(PVOID, PVOID);
HidP_GetCapsFn g_originalHidP_GetCaps = nullptr;
using HidP_GetValueCapsFn = LONG(WINAPI*)(USHORT, PVOID, PUSHORT, PVOID);
HidP_GetValueCapsFn g_originalHidP_GetValueCaps = nullptr;

std::atomic<DWORD64> g_aButtonUntilMs{0};
std::atomic<DWORD64> g_aButtonReleaseUntilMs{0};
std::atomic<DWORD64> g_aButtonStartedMs{0};
std::atomic<DWORD64> g_aButtonMaxUntilMs{0};
std::atomic<bool> g_aButtonMaxLogged{false};
std::atomic<DWORD64> g_retryAfterMs{0};
std::atomic<DWORD64> g_retryUntilMs{0};
std::atomic<DWORD> g_tapAttemptsForArm{0};
std::atomic<DWORD> g_virtualPacket{0x10000};
std::atomic<DWORD> g_hookCalls{0};
std::atomic<DWORD> g_injectedCalls{0};
std::atomic<DWORD> g_xinputSetStateCalls{0};
std::atomic<DWORD> g_xinputCapabilitiesCalls{0};
std::atomic<DWORD> g_xinputKeystrokeCalls{0};
std::atomic<DWORD> g_injectedKeystrokes{0};
std::atomic<DWORD> g_rawInputDataCalls{0};
std::atomic<DWORD> g_rawInputHeaderCalls{0};
std::atomic<DWORD> g_rawInputInputCalls{0};
std::atomic<DWORD> g_rawInputDeviceInfoCalls{0};
std::atomic<DWORD> g_rawInputDeviceListCalls{0};
std::atomic<DWORD> g_rawInputRegisterCalls{0};
std::atomic<DWORD> g_rawInputRegisteredCalls{0};
std::atomic<DWORD> g_hidAttributesCalls{0};
std::atomic<DWORD> g_hidPreparsedCalls{0};
std::atomic<DWORD> g_hidFreePreparsedCalls{0};
std::atomic<DWORD> g_hidGetFeatureCalls{0};
std::atomic<DWORD> g_hidSetFeatureCalls{0};
std::atomic<DWORD> g_hidGetCapsCalls{0};
std::atomic<DWORD> g_hidGetValueCapsCalls{0};
std::atomic<DWORD> g_loadLibraryCalls{0};
std::atomic<DWORD> g_getProcAddressCalls{0};
std::atomic<bool> g_hookArmed{true};
std::atomic<bool> g_hookPending{false};
std::atomic<DWORD64> g_hookTapStartMs{0};
std::atomic<DWORD64> g_hookTapDueMs{0};
std::atomic<DWORD64> g_walkingSinceMs{0};
std::atomic<DWORD> g_lastObservedUserIndex{0};
std::atomic<SHORT> g_lastObservedLeftX{0};
std::atomic<SHORT> g_lastObservedLeftY{0};
std::atomic<SHORT> g_lastObservedLeftMagnitude{0};
std::atomic<WORD> g_lastObservedButtons{0};
std::atomic<DWORD> g_thresholdCrossings{0};
std::atomic<DWORD> g_sharedPlayerReads{0};
std::atomic<DWORD> g_sharedPlayerValidReads{0};
std::atomic<DWORD> g_cdCoreReads{0};
std::atomic<DWORD> g_cdCoreControlledReads{0};
std::atomic<DWORD> g_characterControlReads{0};
std::atomic<DWORD> g_characterControlValidReads{0};
std::atomic<DWORD> g_characterControlStateChanges{0};
std::atomic<DWORD> g_writeTraceEvents{0};
std::atomic<DWORD> g_writeTraceDropped{0};
std::atomic<bool> g_lastRealAButtonDown{false};
std::atomic<bool> g_keystrokeDownPending{false};
std::atomic<bool> g_keystrokeUpPending{false};
std::atomic<DWORD64> g_keystrokeUpDueMs{0};
std::atomic<bool> g_threadStarted{false};
bool g_isTargetProcess = false;
std::atomic<uintptr_t> g_characterControlVtable{0};
constexpr std::string_view kCharacterControlRtti =
    ".?AVClientCharacterControlActorComponent@pa@@";

struct CharacterControlFocusSnapshot {
    bool valid = false;
    uintptr_t component = 0;
    uint32_t dwords[10]{};
};

struct WriteTraceEvent {
    DWORD tick = 0;
    DWORD threadId = 0;
    uintptr_t component = 0;
    uintptr_t field = 0;
    uintptr_t instruction = 0;
    uint32_t before = 0;
    uint32_t after = 0;
    SHORT leftY = 0;
    WORD buttons = 0;
};

constexpr size_t kWriteTraceEventCapacity = 256;
WriteTraceEvent g_writeTraceEventsRing[kWriteTraceEventCapacity]{};
std::atomic<DWORD> g_writeTraceWriteIndex{0};
std::atomic<DWORD> g_writeTraceReadIndex{0};
std::atomic<uintptr_t> g_writeTraceComponent{0};
std::atomic<uintptr_t> g_writeTraceField{0};
std::atomic<uintptr_t> g_writeTracePage{0};
std::atomic<DWORD> g_writeTraceProtect{0};
std::atomic<bool> g_writeTraceGuarded{false};
std::atomic<bool> g_writeTraceInstalling{false};
PVOID g_writeTraceVeh = nullptr;

struct ThreadWriteTraceStep {
    bool active = false;
    bool log = false;
    uintptr_t page = 0;
    DWORD protect = 0;
    uintptr_t component = 0;
    uintptr_t field = 0;
    uintptr_t instruction = 0;
    uint32_t before = 0;
    DWORD threadId = 0;
    SHORT leftY = 0;
    WORD buttons = 0;
};

thread_local ThreadWriteTraceStep g_writeTraceStep{};

const char* ClassifyLocomotionState(const CharacterControlFocusSnapshot& snapshot);

std::string ReplaceExtension(const char* path, const char* extension) {
    std::string value(path);
    const size_t slash = value.find_last_of("\\/");
    const size_t dot = value.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        value.resize(dot);
    }
    value += extension;
    return value;
}

void Log(const char* format, ...) {
    FILE* file = nullptr;
    fopen_s(&file, g_logPath.c_str(), "a");
    if (!file) {
        return;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    fprintf(file, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
            now.wYear,
            now.wMonth,
            now.wDay,
            now.wHour,
            now.wMinute,
            now.wSecond,
            now.wMilliseconds);

    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

void InitializePaths() {
    char modulePath[MAX_PATH]{};
    GetModuleFileNameA(g_module, modulePath, MAX_PATH);
    g_logPath = ReplaceExtension(modulePath, ".log");
    g_iniPath = ReplaceExtension(modulePath, ".ini");
}

std::string BaseName(const char* path) {
    const char* backslash = strrchr(path, '\\');
    const char* slash = strrchr(path, '/');
    const char* base = backslash > slash ? backslash : slash;
    return base == nullptr ? std::string(path) : std::string(base + 1);
}

void LoadSettings() {
    g_settings.enabled = GetPrivateProfileIntA("Settings", "Enabled", 1, g_iniPath.c_str()) != 0;
    g_settings.diagnostics =
        GetPrivateProfileIntA("Settings", "Diagnostics", 1, g_iniPath.c_str()) != 0;
    g_settings.sharedPlayerLogging =
        GetPrivateProfileIntA("Settings", "SharedPlayerLogging", 1, g_iniPath.c_str()) != 0;
    g_settings.writeTrace =
        GetPrivateProfileIntA("Settings", "WriteTrace", 0, g_iniPath.c_str()) != 0;
    g_settings.forwardOn =
        static_cast<SHORT>(GetPrivateProfileIntA("Settings", "ForwardOn", 31130, g_iniPath.c_str()));
    g_settings.forwardOff =
        static_cast<SHORT>(GetPrivateProfileIntA("Settings", "ForwardOff", 2000, g_iniPath.c_str()));
    g_settings.pollRateMs = GetPrivateProfileIntA("Settings", "PollRateMs", 8, g_iniPath.c_str());
    g_settings.tapDurationMs =
        GetPrivateProfileIntA("Settings", "TapDurationMs", 90, g_iniPath.c_str());
    g_settings.tapMaxDurationMs =
        GetPrivateProfileIntA("Settings", "TapMaxDurationMs", 220, g_iniPath.c_str());
    g_settings.releaseDurationMs =
        GetPrivateProfileIntA("Settings", "ReleaseDurationMs", 180, g_iniPath.c_str());
    g_settings.retryDelayMs =
        GetPrivateProfileIntA("Settings", "RetryDelayMs", 250, g_iniPath.c_str());
    g_settings.retryWindowMs =
        GetPrivateProfileIntA("Settings", "RetryWindowMs", 1200, g_iniPath.c_str());
    g_settings.triggerDelayMs =
        GetPrivateProfileIntA("Settings", "TriggerDelayMs", 600, g_iniPath.c_str());
    g_settings.walkingStableMs =
        GetPrivateProfileIntA("Settings", "WalkingStableMs", 60, g_iniPath.c_str());
    g_settings.writeTraceMaxEvents =
        GetPrivateProfileIntA("Settings", "WriteTraceMaxEvents", 200, g_iniPath.c_str());

    if (g_settings.forwardOff > g_settings.forwardOn) {
        g_settings.forwardOff = g_settings.forwardOn;
    }
    if (g_settings.pollRateMs == 0) {
        g_settings.pollRateMs = 8;
    }
    if (g_settings.tapDurationMs == 0) {
        g_settings.tapDurationMs = 90;
    }
    if (g_settings.tapMaxDurationMs < g_settings.tapDurationMs) {
        g_settings.tapMaxDurationMs = g_settings.tapDurationMs;
    }
    if (g_settings.releaseDurationMs == 0) {
        g_settings.releaseDurationMs = 180;
    }
    if (g_settings.writeTraceMaxEvents == 0) {
        g_settings.writeTraceMaxEvents = 200;
    }
}

void TryOpenSharedPlayerMap() {
    if (g_sharedPlayerMap != nullptr && g_sharedPlayer != nullptr) {
        return;
    }

    HANDLE map = OpenFileMappingA(FILE_MAP_READ, FALSE, kSharedPlayerMapName);
    if (map == nullptr) {
        return;
    }

    void* view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(SharedPlayerData));
    if (view == nullptr) {
        CloseHandle(map);
        return;
    }

    g_sharedPlayerMap = map;
    g_sharedPlayer = reinterpret_cast<SharedPlayerData*>(view);
    Log("shared player map opened: name=%s view=%p", kSharedPlayerMapName, view);
}

bool ReadVelocitySnapshot(VelocitySnapshot* snapshot) {
    if (snapshot == nullptr || !g_settings.sharedPlayerLogging) {
        return false;
    }

    TryOpenSharedPlayerMap();
    g_sharedPlayerReads.fetch_add(1);
    if (g_sharedPlayer == nullptr) {
        return false;
    }

    const DWORD now = GetTickCount();
    const uintptr_t base = g_sharedPlayer->playerBase;
    const DWORD updated = g_sharedPlayer->lastUpdateTick;
    const DWORD age = now - updated;
    if (g_sharedPlayer->version != kSharedPlayerVersion || base <= 0x100000 || age > kSharedPlayerFreshMs) {
        snapshot->valid = false;
        snapshot->playerBase = base;
        snapshot->ageMs = age;
        return false;
    }

    __try {
        float* velocity = reinterpret_cast<float*>(base + kVelocityOffset);
        snapshot->x = velocity[0];
        snapshot->z = velocity[1];
        snapshot->y = velocity[2];
        snapshot->w = velocity[3];
        snapshot->valid = true;
        snapshot->playerBase = base;
        snapshot->ageMs = age;
        g_sharedPlayerValidReads.fetch_add(1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snapshot->valid = false;
        snapshot->playerBase = base;
        snapshot->ageMs = age;
        return false;
    }
}

void LogVelocitySnapshot(const char* reason) {
    if (!g_settings.diagnostics) {
        return;
    }

    VelocitySnapshot snap{};
    const bool valid = ReadVelocitySnapshot(&snap);
    if (valid) {
        Log("%s: playerBase=0x%p vel={x=%.4f z=%.4f y=%.4f w=%.4f} ageMs=%lu",
            reason,
            reinterpret_cast<void*>(snap.playerBase),
            snap.x,
            snap.z,
            snap.y,
            snap.w,
            snap.ageMs);
    } else {
        Log("%s: playerBase unavailable base=0x%p ageMs=%lu sharedMap=%s",
            reason,
            reinterpret_cast<void*>(snap.playerBase),
            snap.ageMs,
            g_sharedPlayer == nullptr ? "closed" : "open");
    }
}

void LogCdCoreSnapshot(const char* reason, bool includeActorList) {
    if (!g_settings.diagnostics) {
        return;
    }

    g_cdCoreReads.fetch_add(1);
    const uintptr_t controlled = CDCore::current_controlled_ccoia();
    const uint32_t charIdx = CDCore::current_controlled_character_idx();
    const auto charName = CDCore::current_controlled_character_name();
    const uint64_t generation = CDCore::world_generation();
    if (controlled > 0x10000) {
        g_cdCoreControlledReads.fetch_add(1);
    }

    Log("%s: cdcore controlled=0x%p charIdx=%lu char=%.*s generation=%llu",
        reason,
        reinterpret_cast<void*>(controlled),
        static_cast<unsigned long>(charIdx),
        static_cast<int>(charName.size()),
        charName.data(),
        static_cast<unsigned long long>(generation));

    if (!includeActorList) {
        return;
    }

    CDCore::ActorListDebugEntry entries[12]{};
    const auto summary = CDCore::debug_enumerate_actor_list(entries, 12);
    Log("%s: cdcore actors mgr=0x%p user=0x%p subMgr=0x%p kliff=0x%p controlled=0x%p vec=0x%p child=0x%p list=0x%p rawEntries=%llu",
        reason,
        reinterpret_cast<void*>(summary.mgr),
        reinterpret_cast<void*>(summary.userActor),
        reinterpret_cast<void*>(summary.subMgr),
        reinterpret_cast<void*>(summary.kliffCcoia),
        reinterpret_cast<void*>(summary.controlled),
        reinterpret_cast<void*>(summary.vecData),
        reinterpret_cast<void*>(summary.childContainer),
        reinterpret_cast<void*>(summary.actorList),
        static_cast<unsigned long long>(summary.rawEntries));

    const size_t count = summary.rawEntries < 12 ? summary.rawEntries : 12;
    for (size_t i = 0; i < count; ++i) {
        Log("%s: cdcore actor[%llu] ccoia=0x%p flag=0x%llx identity=0x%08lx",
            reason,
            static_cast<unsigned long long>(i),
            reinterpret_cast<void*>(entries[i].ccoia),
            static_cast<unsigned long long>(entries[i].flag),
            static_cast<unsigned long>(entries[i].identity));
    }
}

bool ReadProcessMemorySeh(uintptr_t address, void* out, size_t size) {
    if (address <= 0x10000 || out == nullptr || size == 0) {
        return false;
    }

    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

SHORT StickMagnitude(SHORT x, SHORT y) {
    const double dx = static_cast<double>(x);
    const double dy = static_cast<double>(y);
    const double magnitude = std::sqrt(dx * dx + dy * dy);
    if (magnitude >= 32767.0) {
        return 32767;
    }
    return static_cast<SHORT>(magnitude + 0.5);
}

void LogBytesLine(const char* reason, const char* label, uintptr_t base, size_t offset, const BYTE* bytes, size_t count) {
    char hex[3 * 16 + 1]{};
    const size_t limited = std::min<size_t>(count, 16);
    size_t pos = 0;
    for (size_t i = 0; i < limited && pos + 3 < sizeof(hex); ++i) {
        pos += std::snprintf(hex + pos, sizeof(hex) - pos, "%02X%s", bytes[i], i + 1 == limited ? "" : " ");
    }
    Log("%s: %s+0x%03llx @0x%p bytes=%s",
        reason,
        label,
        static_cast<unsigned long long>(offset),
        reinterpret_cast<void*>(base + offset),
        hex);
}

void LogFloatLine(const char* reason, const char* label, uintptr_t base, size_t offset, const float* values, size_t count) {
    if (count >= 4) {
        Log("%s: %s+0x%03llx floats={%.5f, %.5f, %.5f, %.5f}",
            reason,
            label,
            static_cast<unsigned long long>(offset),
            values[0],
            values[1],
            values[2],
            values[3]);
    }
}

void LogComponentField16(const char* reason, const char* label, uintptr_t base, size_t offset) {
    BYTE bytes[16]{};
    if (!ReadProcessMemorySeh(base + offset, bytes, sizeof(bytes))) {
        Log("%s: %s+0x%03llx unavailable", reason, label, static_cast<unsigned long long>(offset));
        return;
    }

    uint32_t dwords[4]{};
    float floats[4]{};
    std::memcpy(dwords, bytes, sizeof(dwords));
    std::memcpy(floats, bytes, sizeof(floats));

    char hex[3 * 16 + 1]{};
    size_t pos = 0;
    for (size_t i = 0; i < 16 && pos + 3 < sizeof(hex); ++i) {
        pos += std::snprintf(hex + pos, sizeof(hex) - pos, "%02X%s", bytes[i], i == 15 ? "" : " ");
    }

    Log("%s: %s+0x%03llx bytes=%s u32={0x%08lx,0x%08lx,0x%08lx,0x%08lx} f32={%.5f,%.5f,%.5f,%.5f}",
        reason,
        label,
        static_cast<unsigned long long>(offset),
        hex,
        static_cast<unsigned long>(dwords[0]),
        static_cast<unsigned long>(dwords[1]),
        static_cast<unsigned long>(dwords[2]),
        static_cast<unsigned long>(dwords[3]),
        floats[0],
        floats[1],
        floats[2],
        floats[3]);
}

uintptr_t FindCharacterControlComponent() {
    return CDCore::find_component_in_controlled_actor(kCharacterControlRtti, g_characterControlVtable);
}

bool ReadCharacterControlFocusSnapshot(CharacterControlFocusSnapshot* snapshot) {
    if (snapshot == nullptr) {
        return false;
    }

    g_characterControlReads.fetch_add(1);
    const uintptr_t component = FindCharacterControlComponent();
    snapshot->component = component;
    if (component <= 0x10000) {
        snapshot->valid = false;
        return false;
    }

    if (!ReadProcessMemorySeh(component + 0x088, snapshot->dwords, sizeof(snapshot->dwords))) {
        snapshot->valid = false;
        return false;
    }

    snapshot->valid = true;
    g_characterControlValidReads.fetch_add(1);
    return true;
}

uintptr_t PageBaseForAddress(uintptr_t address) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uintptr_t pageSize = info.dwPageSize != 0 ? info.dwPageSize : 0x1000;
    return address & ~(pageSize - 1);
}

bool ReadU32Seh(uintptr_t address, uint32_t* value) {
    if (value == nullptr) {
        return false;
    }
    return ReadProcessMemorySeh(address, value, sizeof(*value));
}

std::string ModuleRelativeAddress(uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0 ||
        mbi.AllocationBase == nullptr) {
        char raw[32]{};
        std::snprintf(raw, sizeof(raw), "0x%p", reinterpret_cast<void*>(address));
        return raw;
    }

    char modulePath[MAX_PATH]{};
    if (GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), modulePath, MAX_PATH) == 0) {
        char raw[64]{};
        std::snprintf(raw,
            sizeof(raw),
            "0x%p+0x%llx",
            mbi.AllocationBase,
            static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(mbi.AllocationBase)));
        return raw;
    }

    char relative[MAX_PATH + 32]{};
    const std::string baseName = BaseName(modulePath);
    std::snprintf(relative,
        sizeof(relative),
        "%s+0x%llx",
        baseName.c_str(),
        static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(mbi.AllocationBase)));
    return relative;
}

void EnqueueWriteTraceEvent(const ThreadWriteTraceStep& step, uint32_t after) {
    const DWORD eventNumber = g_writeTraceEvents.fetch_add(1) + 1;
    if (eventNumber > g_settings.writeTraceMaxEvents) {
        g_writeTraceDropped.fetch_add(1);
        return;
    }

    const DWORD writeIndex = g_writeTraceWriteIndex.fetch_add(1);
    WriteTraceEvent& event = g_writeTraceEventsRing[writeIndex % kWriteTraceEventCapacity];
    event.tick = GetTickCount();
    event.threadId = step.threadId;
    event.component = step.component;
    event.field = step.field;
    event.instruction = step.instruction;
    event.before = step.before;
    event.after = after;
    event.leftY = step.leftY;
    event.buttons = step.buttons;
}

bool ArmWriteTraceGuard(uintptr_t page, DWORD protect) {
    DWORD oldProtect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(page),
            1,
            protect | PAGE_GUARD,
            &oldProtect) == 0) {
        return false;
    }
    g_writeTraceGuarded.store(true);
    return true;
}

LONG CALLBACK WriteTraceVectoredHandler(PEXCEPTION_POINTERS exceptionInfo) {
    if (exceptionInfo == nullptr || exceptionInfo->ExceptionRecord == nullptr ||
        exceptionInfo->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const DWORD code = exceptionInfo->ExceptionRecord->ExceptionCode;
    if (code == STATUS_SINGLE_STEP && g_writeTraceStep.active) {
        uint32_t after = 0;
        if (g_writeTraceStep.log && ReadU32Seh(g_writeTraceStep.field, &after)) {
            EnqueueWriteTraceEvent(g_writeTraceStep, after);
        }

        ArmWriteTraceGuard(g_writeTraceStep.page, g_writeTraceStep.protect);
        g_writeTraceStep = {};
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code != STATUS_GUARD_PAGE_VIOLATION || !g_settings.writeTrace) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const uintptr_t page = g_writeTracePage.load();
    const uintptr_t field = g_writeTraceField.load();
    const uintptr_t component = g_writeTraceComponent.load();
    const DWORD protect = g_writeTraceProtect.load();
    if (page == 0 || field == 0 || protect == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const ULONG_PTR accessKind = exceptionInfo->ExceptionRecord->ExceptionInformation[0];
    const uintptr_t accessAddress =
        static_cast<uintptr_t>(exceptionInfo->ExceptionRecord->ExceptionInformation[1]);
    const bool touchesTarget = accessAddress < field + sizeof(uint32_t) &&
        accessAddress + sizeof(uint32_t) > field;

    g_writeTraceStep = {};
    g_writeTraceStep.active = true;
    g_writeTraceStep.log = accessKind == 1 && touchesTarget;
    g_writeTraceStep.page = page;
    g_writeTraceStep.protect = protect;
    g_writeTraceStep.component = component;
    g_writeTraceStep.field = field;
    g_writeTraceStep.instruction =
        reinterpret_cast<uintptr_t>(exceptionInfo->ExceptionRecord->ExceptionAddress);
    g_writeTraceStep.threadId = GetCurrentThreadId();
    g_writeTraceStep.leftY = g_lastObservedLeftY.load();
    g_writeTraceStep.buttons = g_lastObservedButtons.load();
    ReadU32Seh(field, &g_writeTraceStep.before);

#if defined(_M_X64)
    exceptionInfo->ContextRecord->EFlags |= 0x100;
#elif defined(_M_IX86)
    exceptionInfo->ContextRecord->EFlags |= 0x100;
#endif
    g_writeTraceGuarded.store(false);
    return EXCEPTION_CONTINUE_EXECUTION;
}

void EnsureWriteTraceInstalled() {
    if (!g_settings.writeTrace || !g_settings.diagnostics) {
        return;
    }
    if (g_writeTraceEvents.load() >= g_settings.writeTraceMaxEvents) {
        return;
    }
    if (g_writeTraceInstalling.exchange(true)) {
        return;
    }

    if (g_writeTraceVeh == nullptr) {
        g_writeTraceVeh = AddVectoredExceptionHandler(1, WriteTraceVectoredHandler);
        Log("write-trace: veh=%p", g_writeTraceVeh);
    }

    CharacterControlFocusSnapshot snapshot{};
    if (!ReadCharacterControlFocusSnapshot(&snapshot)) {
        g_writeTraceInstalling.store(false);
        return;
    }

    const uintptr_t field = snapshot.component + 0x09c;
    const uintptr_t page = PageBaseForAddress(field);
    if (g_writeTraceField.load() == field && g_writeTraceGuarded.load()) {
        g_writeTraceInstalling.store(false);
        return;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(field), &mbi, sizeof(mbi)) == 0) {
        Log("write-trace: VirtualQuery failed field=0x%p error=%lu",
            reinterpret_cast<void*>(field),
            GetLastError());
        g_writeTraceInstalling.store(false);
        return;
    }

    const DWORD baseProtect = mbi.Protect & ~(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
    g_writeTraceComponent.store(snapshot.component);
    g_writeTraceField.store(field);
    g_writeTracePage.store(page);
    g_writeTraceProtect.store(baseProtect);

    if (ArmWriteTraceGuard(page, baseProtect)) {
        Log("write-trace: armed comp=0x%p field=0x%p page=0x%p protect=0x%08lx d09c=0x%08lx locomotion=%s",
            reinterpret_cast<void*>(snapshot.component),
            reinterpret_cast<void*>(field),
            reinterpret_cast<void*>(page),
            static_cast<unsigned long>(baseProtect),
            static_cast<unsigned long>(snapshot.dwords[5]),
            ClassifyLocomotionState(snapshot));
    } else {
        Log("write-trace: VirtualProtect failed page=0x%p protect=0x%08lx error=%lu",
            reinterpret_cast<void*>(page),
            static_cast<unsigned long>(baseProtect),
            GetLastError());
    }

    g_writeTraceInstalling.store(false);
}

void FlushWriteTraceEvents() {
    DWORD readIndex = g_writeTraceReadIndex.load();
    const DWORD writeIndex = g_writeTraceWriteIndex.load();
    while (readIndex != writeIndex) {
        const WriteTraceEvent& event = g_writeTraceEventsRing[readIndex % kWriteTraceEventCapacity];
        CharacterControlFocusSnapshot snapshot{};
        snapshot.valid = true;
        snapshot.component = event.component;
        snapshot.dwords[4] = 0x00040009;
        snapshot.dwords[5] = event.after;

        Log("write-trace: tid=%lu instr=%s abs=0x%p comp=0x%p field=0x%p d09c 0x%08lx -> 0x%08lx locomotion=%s leftY=%d buttons=0x%04x",
            event.threadId,
            ModuleRelativeAddress(event.instruction).c_str(),
            reinterpret_cast<void*>(event.instruction),
            reinterpret_cast<void*>(event.component),
            reinterpret_cast<void*>(event.field),
            static_cast<unsigned long>(event.before),
            static_cast<unsigned long>(event.after),
            ClassifyLocomotionState(snapshot),
            event.leftY,
            event.buttons);

        ++readIndex;
        g_writeTraceReadIndex.store(readIndex);
    }
}

void ShutdownWriteTrace() {
    if (g_writeTraceVeh != nullptr) {
        RemoveVectoredExceptionHandler(g_writeTraceVeh);
        g_writeTraceVeh = nullptr;
    }
}

void LogCharacterControlFocus(const char* reason, bool force) {
    if (!g_settings.diagnostics) {
        return;
    }

    static std::mutex mutex;
    static bool hasPrevious = false;
    static CharacterControlFocusSnapshot previous{};

    CharacterControlFocusSnapshot current{};
    if (!ReadCharacterControlFocusSnapshot(&current)) {
        if (force) {
            Log("%s: ccc-focus unavailable component=0x%p leftY=%d buttons=0x%04x armed=%d pending=%d autoA=%d",
                reason,
                reinterpret_cast<void*>(current.component),
                g_lastObservedLeftY.load(),
                g_lastObservedButtons.load(),
                g_hookArmed.load() ? 1 : 0,
                g_hookPending.load() ? 1 : 0,
                GetTickCount64() < g_aButtonUntilMs.load() ? 1 : 0);
        }
        return;
    }

    std::lock_guard<std::mutex> lock(mutex);
    bool changed = !hasPrevious || previous.component != current.component;
    uint32_t xorDwords[10]{};
    if (hasPrevious && previous.component == current.component) {
        for (size_t i = 0; i < 10; ++i) {
            xorDwords[i] = previous.dwords[i] ^ current.dwords[i];
            changed = changed || xorDwords[i] != 0;
        }
    }

    if (!force && !changed) {
        return;
    }

    if (changed) {
        g_characterControlStateChanges.fetch_add(1);
    }

    const DWORD64 now = GetTickCount64();
    Log("%s: ccc-focus%s comp=0x%p locomotion=%s leftY=%d buttons=0x%04x realA=%d autoA=%d armed=%d pending=%d d088=0x%08lx d08c=0x%08lx d090=0x%08lx d094=0x%08lx d098=0x%08lx d09c=0x%08lx d0a0=0x%08lx d0a4=0x%08lx d0a8=0x%08lx d0ac=0x%08lx",
        reason,
        changed ? " changed" : "",
        reinterpret_cast<void*>(current.component),
        ClassifyLocomotionState(current),
        g_lastObservedLeftY.load(),
        g_lastObservedButtons.load(),
        (g_lastObservedButtons.load() & XINPUT_GAMEPAD_A) != 0 ? 1 : 0,
        now < g_aButtonUntilMs.load() ? 1 : 0,
        g_hookArmed.load() ? 1 : 0,
        g_hookPending.load() ? 1 : 0,
        static_cast<unsigned long>(current.dwords[0]),
        static_cast<unsigned long>(current.dwords[1]),
        static_cast<unsigned long>(current.dwords[2]),
        static_cast<unsigned long>(current.dwords[3]),
        static_cast<unsigned long>(current.dwords[4]),
        static_cast<unsigned long>(current.dwords[5]),
        static_cast<unsigned long>(current.dwords[6]),
        static_cast<unsigned long>(current.dwords[7]),
        static_cast<unsigned long>(current.dwords[8]),
        static_cast<unsigned long>(current.dwords[9]));

    if (hasPrevious && previous.component == current.component && changed) {
        Log("%s: ccc-focus-xor d088=0x%08lx d08c=0x%08lx d090=0x%08lx d094=0x%08lx d098=0x%08lx d09c=0x%08lx d0a0=0x%08lx d0a4=0x%08lx d0a8=0x%08lx d0ac=0x%08lx",
            reason,
            static_cast<unsigned long>(xorDwords[0]),
            static_cast<unsigned long>(xorDwords[1]),
            static_cast<unsigned long>(xorDwords[2]),
            static_cast<unsigned long>(xorDwords[3]),
            static_cast<unsigned long>(xorDwords[4]),
            static_cast<unsigned long>(xorDwords[5]),
            static_cast<unsigned long>(xorDwords[6]),
            static_cast<unsigned long>(xorDwords[7]),
            static_cast<unsigned long>(xorDwords[8]),
            static_cast<unsigned long>(xorDwords[9]));
    }

    previous = current;
    hasPrevious = true;
}

bool IsCharacterControlReadyForJogTap() {
    CharacterControlFocusSnapshot snapshot{};
    if (!ReadCharacterControlFocusSnapshot(&snapshot)) {
        return false;
    }

    const uint32_t state = snapshot.dwords[4];
    const uint32_t movement = snapshot.dwords[5];
    const bool acceptedForwardMovement = state == 0x00040009 &&
        (movement & 0x00020000) != 0;
    if (acceptedForwardMovement && g_settings.diagnostics) {
        Log("jog-ready-state: d098=0x%08lx d09c=0x%08lx leftY=%d",
            static_cast<unsigned long>(state),
            static_cast<unsigned long>(movement),
            g_lastObservedLeftY.load());
    }
    return acceptedForwardMovement;
}

bool IsWalkingForwardState(const CharacterControlFocusSnapshot& snapshot) {
    if (!snapshot.valid) {
        return false;
    }

    const uint32_t state = snapshot.dwords[4];
    const uint32_t movement = snapshot.dwords[5];
    const bool movingForward = (movement & 0x00020000) != 0;
    const bool alreadyPromoted = (movement & 0x001c0000) != 0;
    return state == 0x00040009 && movingForward && !alreadyPromoted;
}

bool IsJogPromotionState(const CharacterControlFocusSnapshot& snapshot) {
    if (!snapshot.valid) {
        return false;
    }

    const uint32_t state = snapshot.dwords[4];
    const uint32_t movement = snapshot.dwords[5];
    return state == 0x00040009 && (movement & 0x001c0000) != 0;
}

void CancelVirtualATap(const char* reason);

void CancelVirtualATap(const char* reason) {
    const DWORD64 now = GetTickCount64();
    const bool hadTap = now < g_aButtonUntilMs.load() || now < g_aButtonReleaseUntilMs.load() ||
        g_aButtonMaxUntilMs.load() != 0 || g_retryAfterMs.load() != 0;
    g_aButtonUntilMs.store(0);
    g_aButtonReleaseUntilMs.store(0);
    g_aButtonMaxUntilMs.store(0);
    g_retryAfterMs.store(0);
    g_retryUntilMs.store(0);
    g_hookPending.store(false);
    g_walkingSinceMs.store(0);
    if (hadTap && g_settings.diagnostics) {
        Log("auto-a canceled: reason=%s leftX=%d leftY=%d stick=%d",
            reason,
            g_lastObservedLeftX.load(),
            g_lastObservedLeftY.load(),
            g_lastObservedLeftMagnitude.load());
    }
}

const char* ClassifyLocomotionState(const CharacterControlFocusSnapshot& snapshot) {
    if (!snapshot.valid) {
        return "Unavailable";
    }

    const uint32_t state = snapshot.dwords[4];
    const uint32_t movement = snapshot.dwords[5];
    if (state == 0x00040009) {
        if ((movement & 0x00080000) != 0) {
            return "Sprint";
        }
        if ((movement & 0x00040000) != 0) {
            return "Run";
        }
        if ((movement & 0x00020000) != 0) {
            return "Walk";
        }
        if ((movement & 0x00010000) != 0) {
            return "Idle";
        }
        if ((movement & 0x00004000) != 0) {
            return "Stopped";
        }
    }

    if (state == 0x00040109 || state == 0x00040101 || state == 0x04000003 ||
        state == 0x04000001) {
        return "Stopped";
    }

    return "Unknown";
}

void LogCharacterControlProbe(const char* reason, bool includeDump);

void QueueVirtualATap(DWORD userIndex, SHORT forward, WORD buttonsBefore, DWORD packet, const char* mode) {
    const DWORD64 now = GetTickCount64();
    g_tapAttemptsForArm.fetch_add(1);
    g_aButtonStartedMs.store(now);
    g_aButtonMaxUntilMs.store(now + g_settings.tapMaxDurationMs);
    g_aButtonMaxLogged.store(false);
    g_aButtonUntilMs.store(now + g_settings.tapDurationMs);
    g_aButtonReleaseUntilMs.store(now + g_settings.tapDurationMs + g_settings.releaseDurationMs);
    g_keystrokeDownPending.store(true);
    g_keystrokeUpPending.store(true);
    g_keystrokeUpDueMs.store(now + g_settings.tapDurationMs);
    g_hookArmed.store(false);
    g_hookPending.store(false);
    Log("hook queued %s A tap: user=%lu leftX=%d leftY=%d stick=%d durationMs=%lu maxDurationMs=%lu releaseMs=%lu delayMs=%lu buttonsBefore=0x%04x packet=%lu",
        mode,
        userIndex,
        g_lastObservedLeftX.load(),
        g_lastObservedLeftY.load(),
        forward,
        g_settings.tapDurationMs,
        g_settings.tapMaxDurationMs,
        g_settings.releaseDurationMs,
        g_settings.triggerDelayMs,
        buttonsBefore,
        packet);
    LogVelocitySnapshot("tap velocity");
    LogCdCoreSnapshot("tap", false);
    LogCharacterControlProbe("tap", false);
    LogCharacterControlFocus("tap-queued", true);
}

void LogCharacterControlProbe(const char* reason, bool includeDump) {
    if (!g_settings.diagnostics) {
        return;
    }

    g_characterControlReads.fetch_add(1);
    const uintptr_t component = FindCharacterControlComponent();
    if (component <= 0x10000) {
        Log("%s: character-control unavailable", reason);
        return;
    }

    g_characterControlValidReads.fetch_add(1);
    uintptr_t vtable = 0;
    ReadProcessMemorySeh(component, &vtable, sizeof(vtable));
    Log("%s: character-control component=0x%p vtable=0x%p cachedVtable=0x%p",
        reason,
        reinterpret_cast<void*>(component),
        reinterpret_cast<void*>(vtable),
        reinterpret_cast<void*>(g_characterControlVtable.load()));

    LogComponentField16(reason, "ccc", component, 0x090);
    LogComponentField16(reason, "ccc", component, 0x160);
    LogComponentField16(reason, "ccc", component, 0x170);
    LogComponentField16(reason, "ccc", component, 0x180);
    LogComponentField16(reason, "ccc", component, 0x190);
    LogComponentField16(reason, "ccc", component, 0x1D0);
    LogComponentField16(reason, "ccc", component, 0x1E0);
    LogComponentField16(reason, "ccc", component, 0x1F0);

    if (!includeDump) {
        return;
    }

    constexpr size_t kDumpSize = 0x200;
    BYTE bytes[kDumpSize]{};
    if (!ReadProcessMemorySeh(component, bytes, sizeof(bytes))) {
        Log("%s: character-control dump failed at 0x%p", reason, reinterpret_cast<void*>(component));
        return;
    }

    for (size_t offset = 0; offset < kDumpSize; offset += 0x10) {
        LogBytesLine(reason, "ccc", component, offset, bytes + offset, 16);
    }

    for (size_t offset = 0; offset + sizeof(float) * 4 <= kDumpSize; offset += 0x10) {
        float values[4]{};
        std::memcpy(values, bytes + offset, sizeof(values));
        bool interesting = false;
        for (float value : values) {
            const float absValue = value < 0.0f ? -value : value;
            if (absValue > 0.0001f && absValue < 10000.0f) {
                interesting = true;
                break;
            }
        }
        if (interesting) {
            LogFloatLine(reason, "ccc", component, offset, values, 4);
        }
    }
}

DWORD WINAPI HookedXInputGetState(DWORD userIndex, XINPUT_STATE* state) {
    const DWORD result = g_originalXInputGetState(userIndex, state);
    if (result == ERROR_SUCCESS && state != nullptr) {
        g_hookCalls.fetch_add(1);

        const DWORD64 now = GetTickCount64();
        const SHORT leftX = state->Gamepad.sThumbLX;
        const SHORT leftY = state->Gamepad.sThumbLY;
        const SHORT stickMagnitude = StickMagnitude(leftX, leftY);
        const SHORT previousMagnitude = g_lastObservedLeftMagnitude.exchange(stickMagnitude);
        g_lastObservedLeftX.store(leftX);
        g_lastObservedLeftY.store(leftY);
        g_lastObservedUserIndex.store(userIndex);
        g_lastObservedButtons.store(state->Gamepad.wButtons);
        const bool realAButtonDown = (state->Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;
        const bool previousRealAButtonDown = g_lastRealAButtonDown.exchange(realAButtonDown);

        if (g_settings.diagnostics && realAButtonDown != previousRealAButtonDown) {
            Log("real A %s: user=%lu leftX=%d leftY=%d stick=%d buttons=0x%04x packet=%lu",
                realAButtonDown ? "down" : "up",
                userIndex,
                leftX,
                leftY,
                stickMagnitude,
                state->Gamepad.wButtons,
                state->dwPacketNumber);
            LogCdCoreSnapshot(realAButtonDown ? "real-a-down" : "real-a-up", false);
            LogCharacterControlProbe(realAButtonDown ? "real-a-down" : "real-a-up", false);
            LogCharacterControlFocus(realAButtonDown ? "real-a-down" : "real-a-up", true);
        }

        if (g_settings.diagnostics && previousMagnitude < g_settings.forwardOn &&
            stickMagnitude >= g_settings.forwardOn) {
            g_thresholdCrossings.fetch_add(1);
            CharacterControlFocusSnapshot thresholdSnapshot{};
            ReadCharacterControlFocusSnapshot(&thresholdSnapshot);
            const bool gameplayCandidate = IsWalkingForwardState(thresholdSnapshot) ||
                IsJogPromotionState(thresholdSnapshot);
            Log("threshold crossed: user=%lu leftX=%d leftY=%d stick=%d prevStick=%d gameplay=%d buttons=0x%04x packet=%lu",
                userIndex,
                leftX,
                leftY,
                stickMagnitude,
                previousMagnitude,
                gameplayCandidate ? 1 : 0,
                state->Gamepad.wButtons,
                state->dwPacketNumber);
            if (gameplayCandidate) {
                LogVelocitySnapshot("threshold velocity");
                LogCdCoreSnapshot("threshold", false);
                LogCharacterControlProbe("threshold", false);
                LogCharacterControlFocus("threshold", true);
            } else {
                Log("threshold suppressed: comp=0x%p locomotion=%s d098=0x%08lx d09c=0x%08lx",
                    reinterpret_cast<void*>(thresholdSnapshot.component),
                    ClassifyLocomotionState(thresholdSnapshot),
                    thresholdSnapshot.valid ? static_cast<unsigned long>(thresholdSnapshot.dwords[4]) : 0,
                    thresholdSnapshot.valid ? static_cast<unsigned long>(thresholdSnapshot.dwords[5]) : 0);
            }
        }

        if (!g_settings.enabled) {
            g_hookArmed.store(true);
            g_hookPending.store(false);
            g_walkingSinceMs.store(0);
            g_retryAfterMs.store(0);
            g_retryUntilMs.store(0);
            g_tapAttemptsForArm.store(0);
        } else if (stickMagnitude <= g_settings.forwardOff) {
            const bool wasArmed = g_hookArmed.load();
            g_hookArmed.store(true);
            g_hookPending.store(false);
            g_walkingSinceMs.store(0);
            g_retryAfterMs.store(0);
            g_retryUntilMs.store(0);
            g_tapAttemptsForArm.store(0);
            if (!wasArmed) {
                Log("hook re-armed: user=%lu leftX=%d leftY=%d stick=%d",
                    userIndex,
                    leftX,
                    leftY,
                    stickMagnitude);
                LogCharacterControlFocus("re-armed", true);
            }
        }

        if (now < g_aButtonUntilMs.load()) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_A;
            state->dwPacketNumber += g_virtualPacket.fetch_add(1);
            g_injectedCalls.fetch_add(1);
            const DWORD injected = g_injectedCalls.load();
            if (g_settings.diagnostics && injected <= 4) {
                LogCharacterControlFocus("auto-a-visible", true);
            }
        } else if (now < g_aButtonReleaseUntilMs.load()) {
            state->dwPacketNumber += g_virtualPacket.fetch_add(1);
        }
    }
    return result;
}

DWORD WINAPI HookedXInputGetKeystroke(DWORD userIndex, DWORD reserved, PXINPUT_KEYSTROKE keystroke) {
    g_xinputKeystrokeCalls.fetch_add(1);

    if (g_settings.enabled && keystroke != nullptr) {
        const DWORD64 now = GetTickCount64();
        bool expected = true;
        if (g_keystrokeDownPending.compare_exchange_strong(expected, false)) {
            std::memset(keystroke, 0, sizeof(*keystroke));
            keystroke->VirtualKey = VK_PAD_A;
            keystroke->Flags = XINPUT_KEYSTROKE_KEYDOWN;
            keystroke->UserIndex = static_cast<BYTE>(userIndex);
            g_injectedKeystrokes.fetch_add(1);
            Log("XInputGetKeystroke injected A down: user=%lu", userIndex);
            return ERROR_SUCCESS;
        }

        expected = true;
        if (now >= g_keystrokeUpDueMs.load() &&
            g_keystrokeUpPending.compare_exchange_strong(expected, false)) {
            std::memset(keystroke, 0, sizeof(*keystroke));
            keystroke->VirtualKey = VK_PAD_A;
            keystroke->Flags = XINPUT_KEYSTROKE_KEYUP;
            keystroke->UserIndex = static_cast<BYTE>(userIndex);
            g_injectedKeystrokes.fetch_add(1);
            Log("XInputGetKeystroke injected A up: user=%lu", userIndex);
            return ERROR_SUCCESS;
        }
    }

    const DWORD result = g_originalXInputGetKeystroke(userIndex, reserved, keystroke);
    const DWORD count = g_xinputKeystrokeCalls.load();
    if (count <= 8 && result == ERROR_SUCCESS && keystroke != nullptr) {
        Log("XInputGetKeystroke original: user=%lu vk=0x%04x flags=0x%04x hid=0x%02x",
            userIndex,
            keystroke->VirtualKey,
            keystroke->Flags,
            keystroke->HidCode);
    }
    return result;
}

DWORD WINAPI HookedXInputSetState(DWORD userIndex, XINPUT_VIBRATION* vibration) {
    const DWORD count = g_xinputSetStateCalls.fetch_add(1) + 1;
    if (count <= 8 && vibration != nullptr) {
        Log("XInputSetState: user=%lu leftMotor=%u rightMotor=%u",
            userIndex,
            vibration->wLeftMotorSpeed,
            vibration->wRightMotorSpeed);
    }
    return g_originalXInputSetState(userIndex, vibration);
}

DWORD WINAPI HookedXInputGetCapabilities(DWORD userIndex, DWORD flags, XINPUT_CAPABILITIES* capabilities) {
    const DWORD result = g_originalXInputGetCapabilities(userIndex, flags, capabilities);
    const DWORD count = g_xinputCapabilitiesCalls.fetch_add(1) + 1;
    if (count <= 8) {
        Log("XInputGetCapabilities: user=%lu flags=0x%08lx result=%lu type=%u subtype=%u",
            userIndex,
            flags,
            result,
            result == ERROR_SUCCESS && capabilities != nullptr ? capabilities->Type : 0,
            result == ERROR_SUCCESS && capabilities != nullptr ? capabilities->SubType : 0);
    }
    return result;
}

UINT WINAPI HookedGetRawInputData(HRAWINPUT rawInput, UINT command, LPVOID data, PUINT size, UINT headerSize) {
    const UINT result = g_originalGetRawInputData(rawInput, command, data, size, headerSize);
    g_rawInputDataCalls.fetch_add(1);
    if (command == RID_HEADER) {
        g_rawInputHeaderCalls.fetch_add(1);
    } else if (command == RID_INPUT) {
        g_rawInputInputCalls.fetch_add(1);
    }

    const DWORD count = g_rawInputDataCalls.load();
    if (count <= 12 || (count % 5000) == 0) {
        DWORD type = 0xffffffff;
        HANDLE device = nullptr;
        if (command == RID_INPUT && data != nullptr && result != static_cast<UINT>(-1)) {
            auto* raw = reinterpret_cast<RAWINPUT*>(data);
            type = raw->header.dwType;
            device = raw->header.hDevice;
        } else if (command == RID_HEADER && data != nullptr && result != static_cast<UINT>(-1)) {
            auto* header = reinterpret_cast<RAWINPUTHEADER*>(data);
            type = header->dwType;
            device = header->hDevice;
        }
        Log("GetRawInputData: count=%lu command=0x%08x result=%u size=%u type=%lu device=%p",
            count,
            command,
            result,
            size != nullptr ? *size : 0,
            type,
            device);
    }
    return result;
}

UINT WINAPI HookedGetRawInputDeviceInfoW(HANDLE device, UINT command, LPVOID data, PUINT size) {
    const UINT result = g_originalGetRawInputDeviceInfoW(device, command, data, size);
    const DWORD count = g_rawInputDeviceInfoCalls.fetch_add(1) + 1;
    if (count <= 24) {
        Log("GetRawInputDeviceInfoW: device=%p command=0x%08x result=%u size=%u",
            device,
            command,
            result,
            size != nullptr ? *size : 0);
    }
    return result;
}

UINT WINAPI HookedGetRawInputDeviceList(PRAWINPUTDEVICELIST devices, PUINT deviceCount, UINT size) {
    const UINT result = g_originalGetRawInputDeviceList(devices, deviceCount, size);
    const DWORD count = g_rawInputDeviceListCalls.fetch_add(1) + 1;
    if (count <= 8) {
        Log("GetRawInputDeviceList: result=%u requestedCount=%u entrySize=%u",
            result,
            deviceCount != nullptr ? *deviceCount : 0,
            size);
    }
    return result;
}

BOOL WINAPI HookedRegisterRawInputDevices(PCRAWINPUTDEVICE devices, UINT numDevices, UINT size) {
    const BOOL result = g_originalRegisterRawInputDevices(devices, numDevices, size);
    g_rawInputRegisterCalls.fetch_add(1);
    Log("RegisterRawInputDevices: numDevices=%u size=%u result=%d lastError=%lu",
        numDevices,
        size,
        result,
        result ? 0 : GetLastError());
    for (UINT i = 0; devices != nullptr && i < numDevices && i < 16; ++i) {
        Log("  raw device[%u]: usagePage=0x%04x usage=0x%04x flags=0x%08lx hwnd=%p",
            i,
            devices[i].usUsagePage,
            devices[i].usUsage,
            devices[i].dwFlags,
            devices[i].hwndTarget);
    }
    return result;
}

UINT WINAPI HookedGetRegisteredRawInputDevices(PRAWINPUTDEVICE devices, PUINT numDevices, UINT size) {
    const UINT result = g_originalGetRegisteredRawInputDevices(devices, numDevices, size);
    const DWORD count = g_rawInputRegisteredCalls.fetch_add(1) + 1;
    if (count <= 8) {
        Log("GetRegisteredRawInputDevices: result=%u count=%u size=%u",
            result,
            numDevices != nullptr ? *numDevices : 0,
            size);
    }
    return result;
}

BOOLEAN WINAPI HookedHidD_GetAttributes(HANDLE device, PVOID attributes) {
    const BOOLEAN result = g_originalHidD_GetAttributes(device, attributes);
    const DWORD count = g_hidAttributesCalls.fetch_add(1) + 1;
    if (count <= 24) {
        Log("HidD_GetAttributes: device=%p result=%u", device, result);
    }
    return result;
}

BOOLEAN WINAPI HookedHidD_GetPreparsedData(HANDLE device, PVOID* preparsedData) {
    const BOOLEAN result = g_originalHidD_GetPreparsedData(device, preparsedData);
    const DWORD count = g_hidPreparsedCalls.fetch_add(1) + 1;
    if (count <= 24) {
        Log("HidD_GetPreparsedData: device=%p result=%u data=%p",
            device,
            result,
            preparsedData != nullptr ? *preparsedData : nullptr);
    }
    return result;
}

BOOLEAN WINAPI HookedHidD_FreePreparsedData(PVOID preparsedData) {
    g_hidFreePreparsedCalls.fetch_add(1);
    return g_originalHidD_FreePreparsedData(preparsedData);
}

BOOLEAN WINAPI HookedHidD_GetFeature(HANDLE device, PVOID reportBuffer, ULONG reportBufferLength) {
    const BOOLEAN result = g_originalHidD_GetFeature(device, reportBuffer, reportBufferLength);
    const DWORD count = g_hidGetFeatureCalls.fetch_add(1) + 1;
    if (count <= 24) {
        Log("HidD_GetFeature: device=%p length=%lu result=%u", device, reportBufferLength, result);
    }
    return result;
}

BOOLEAN WINAPI HookedHidD_SetFeature(HANDLE device, PVOID reportBuffer, ULONG reportBufferLength) {
    const BOOLEAN result = g_originalHidD_SetFeature(device, reportBuffer, reportBufferLength);
    const DWORD count = g_hidSetFeatureCalls.fetch_add(1) + 1;
    if (count <= 24) {
        Log("HidD_SetFeature: device=%p length=%lu result=%u", device, reportBufferLength, result);
    }
    return result;
}

LONG WINAPI HookedHidP_GetCaps(PVOID preparsedData, PVOID capabilities) {
    const LONG result = g_originalHidP_GetCaps(preparsedData, capabilities);
    const DWORD count = g_hidGetCapsCalls.fetch_add(1) + 1;
    if (count <= 24) {
        Log("HidP_GetCaps: data=%p result=0x%08lx", preparsedData, result);
    }
    return result;
}

LONG WINAPI HookedHidP_GetValueCaps(USHORT reportType, PVOID valueCaps, PUSHORT valueCapsLength, PVOID preparsedData) {
    const LONG result = g_originalHidP_GetValueCaps(reportType, valueCaps, valueCapsLength, preparsedData);
    const DWORD count = g_hidGetValueCapsCalls.fetch_add(1) + 1;
    if (count <= 24) {
        Log("HidP_GetValueCaps: reportType=%u length=%u result=0x%08lx",
            reportType,
            valueCapsLength != nullptr ? *valueCapsLength : 0,
            result);
    }
    return result;
}

FARPROC WINAPI HookedGetProcAddress(HMODULE module, LPCSTR procName) {
    FARPROC proc = g_originalGetProcAddress(module, procName);
    g_getProcAddressCalls.fetch_add(1);
    if (procName != nullptr && reinterpret_cast<ULONG_PTR>(procName) <= 0xffff) {
        const WORD ordinal = static_cast<WORD>(reinterpret_cast<ULONG_PTR>(procName));
        if (ordinal == 2 && proc != nullptr &&
            proc != reinterpret_cast<FARPROC>(&HookedXInputGetState)) {
            g_originalXInputGetState = reinterpret_cast<XInputGetStateFn>(proc);
            Log("GetProcAddress intercepted XInputGetState ordinal 2: %p", proc);
            return reinterpret_cast<FARPROC>(&HookedXInputGetState);
        }
        if (ordinal == 3 && proc != nullptr &&
            proc != reinterpret_cast<FARPROC>(&HookedXInputSetState)) {
            g_originalXInputSetState = reinterpret_cast<XInputSetStateFn>(proc);
            Log("GetProcAddress intercepted XInputSetState ordinal 3: %p", proc);
            return reinterpret_cast<FARPROC>(&HookedXInputSetState);
        }
        if (ordinal == 4 && proc != nullptr &&
            proc != reinterpret_cast<FARPROC>(&HookedXInputGetCapabilities)) {
            g_originalXInputGetCapabilities = reinterpret_cast<XInputGetCapabilitiesFn>(proc);
            Log("GetProcAddress intercepted XInputGetCapabilities ordinal 4: %p", proc);
            return reinterpret_cast<FARPROC>(&HookedXInputGetCapabilities);
        }
        if (ordinal == 8 && proc != nullptr &&
            proc != reinterpret_cast<FARPROC>(&HookedXInputGetKeystroke)) {
            g_originalXInputGetKeystroke = reinterpret_cast<XInputGetKeystrokeFn>(proc);
            Log("GetProcAddress intercepted XInputGetKeystroke ordinal 8: %p", proc);
            return reinterpret_cast<FARPROC>(&HookedXInputGetKeystroke);
        }
    }
    if (procName != nullptr &&
        reinterpret_cast<ULONG_PTR>(procName) > 0xffff &&
        strcmp(procName, "XInputGetState") == 0 &&
        proc != nullptr &&
        proc != reinterpret_cast<FARPROC>(&HookedXInputGetState)) {
        g_originalXInputGetState = reinterpret_cast<XInputGetStateFn>(proc);
        Log("GetProcAddress intercepted XInputGetState: %p", proc);
        return reinterpret_cast<FARPROC>(&HookedXInputGetState);
    }
    if (procName != nullptr &&
        reinterpret_cast<ULONG_PTR>(procName) > 0xffff &&
        strcmp(procName, "XInputGetKeystroke") == 0 &&
        proc != nullptr &&
        proc != reinterpret_cast<FARPROC>(&HookedXInputGetKeystroke)) {
        g_originalXInputGetKeystroke = reinterpret_cast<XInputGetKeystrokeFn>(proc);
        Log("GetProcAddress intercepted XInputGetKeystroke: %p", proc);
        return reinterpret_cast<FARPROC>(&HookedXInputGetKeystroke);
    }
    if (procName != nullptr && reinterpret_cast<ULONG_PTR>(procName) > 0xffff &&
        (strstr(procName, "XInput") != nullptr ||
         strstr(procName, "RawInput") != nullptr ||
         strstr(procName, "Hid") != nullptr ||
         strstr(procName, "HID") != nullptr)) {
        Log("GetProcAddress: module=%p proc=%s result=%p", module, procName, proc);
    }
    return proc;
}

HMODULE WINAPI HookedLoadLibraryA(LPCSTR fileName) {
    HMODULE module = g_originalLoadLibraryA(fileName);
    g_loadLibraryCalls.fetch_add(1);
    Log("LoadLibraryA: file=%s module=%p", fileName != nullptr ? fileName : "(null)", module);
    return module;
}

HMODULE WINAPI HookedLoadLibraryW(LPCWSTR fileName) {
    HMODULE module = g_originalLoadLibraryW(fileName);
    g_loadLibraryCalls.fetch_add(1);
    Log("LoadLibraryW: file=%ls module=%p", fileName != nullptr ? fileName : L"(null)", module);
    return module;
}

HMODULE WINAPI HookedLoadLibraryExA(LPCSTR fileName, HANDLE file, DWORD flags) {
    HMODULE module = g_originalLoadLibraryExA(fileName, file, flags);
    g_loadLibraryCalls.fetch_add(1);
    Log("LoadLibraryExA: file=%s flags=0x%08lx module=%p",
        fileName != nullptr ? fileName : "(null)",
        flags,
        module);
    return module;
}

HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR fileName, HANDLE file, DWORD flags) {
    HMODULE module = g_originalLoadLibraryExW(fileName, file, flags);
    g_loadLibraryCalls.fetch_add(1);
    Log("LoadLibraryExW: file=%ls flags=0x%08lx module=%p",
        fileName != nullptr ? fileName : L"(null)",
        flags,
        module);
    return module;
}

bool HookModuleImport(HMODULE module, const char* importedFunction, void* replacement, void** original) {
    if (!module) {
        return false;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<BYTE*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const IMAGE_DATA_DIRECTORY& imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0) {
        return false;
    }

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        reinterpret_cast<BYTE*>(module) + imports.VirtualAddress);

    bool hooked = false;
    for (; descriptor->Name != 0; ++descriptor) {
        if (descriptor->OriginalFirstThunk == 0) {
            continue;
        }

        auto* originalThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            reinterpret_cast<BYTE*>(module) + descriptor->OriginalFirstThunk);
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            reinterpret_cast<BYTE*>(module) + descriptor->FirstThunk);

        for (; originalThunk->u1.AddressOfData != 0 && thunk->u1.Function != 0; ++originalThunk, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(originalThunk->u1.Ordinal)) {
                continue;
            }

            auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                reinterpret_cast<BYTE*>(module) + originalThunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(importByName->Name), importedFunction) != 0) {
                continue;
            }

            auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            *original = *slot;

            DWORD oldProtect = 0;
            if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                *slot = replacement;
                VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
                hooked = true;
            }
        }
    }

    return hooked;
}

bool HookModuleOrdinalImport(
    HMODULE module,
    const char* targetDll,
    WORD ordinal,
    void* replacement,
    void** original) {
    if (!module) {
        return false;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<BYTE*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const IMAGE_DATA_DIRECTORY& imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0) {
        return false;
    }

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        reinterpret_cast<BYTE*>(module) + imports.VirtualAddress);

    bool hooked = false;
    for (; descriptor->Name != 0; ++descriptor) {
        if (descriptor->OriginalFirstThunk == 0) {
            continue;
        }

        const char* dllName = reinterpret_cast<const char*>(
            reinterpret_cast<BYTE*>(module) + descriptor->Name);
        if (_stricmp(dllName, targetDll) != 0) {
            continue;
        }

        auto* originalThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            reinterpret_cast<BYTE*>(module) + descriptor->OriginalFirstThunk);
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            reinterpret_cast<BYTE*>(module) + descriptor->FirstThunk);

        for (; originalThunk->u1.AddressOfData != 0 && thunk->u1.Function != 0; ++originalThunk, ++thunk) {
            if (!IMAGE_SNAP_BY_ORDINAL(originalThunk->u1.Ordinal) ||
                IMAGE_ORDINAL(originalThunk->u1.Ordinal) != ordinal) {
                continue;
            }

            auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            *original = *slot;

            DWORD oldProtect = 0;
            if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                *slot = replacement;
                VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
                hooked = true;
            }
        }
    }

    return hooked;
}

DWORD FindController() {
    XINPUT_STATE state{};
    for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
        if (XInputGetState(index, &state) == ERROR_SUCCESS) {
            return index;
        }
    }
    return XUSER_MAX_COUNT;
}

void PollWalkingJogTrigger() {
    if (!g_settings.enabled) {
        return;
    }

    const DWORD64 now = GetTickCount64();
    const SHORT stick = g_lastObservedLeftMagnitude.load();
    if (stick < g_settings.forwardOn || g_hookPending.load()) {
        g_walkingSinceMs.store(0);
        return;
    }

    CharacterControlFocusSnapshot snapshot{};
    const bool walking = ReadCharacterControlFocusSnapshot(&snapshot) && IsWalkingForwardState(snapshot);
    if (!walking) {
        if (snapshot.valid && IsJogPromotionState(snapshot)) {
            g_retryAfterMs.store(0);
            g_retryUntilMs.store(0);
        }
        g_walkingSinceMs.store(0);
        return;
    }

    const DWORD64 retryAfter = g_retryAfterMs.load();
    if (retryAfter != 0) {
        const DWORD64 retryUntil = g_retryUntilMs.load();
        if (now >= retryAfter && now <= retryUntil && g_tapAttemptsForArm.load() < 2) {
            g_retryAfterMs.store(0);
            g_retryUntilMs.store(0);
            Log("walking retry ready: locomotion=%s leftX=%d leftY=%d stick=%d d098=0x%08lx d09c=0x%08lx",
                ClassifyLocomotionState(snapshot),
                g_lastObservedLeftX.load(),
                g_lastObservedLeftY.load(),
                stick,
                static_cast<unsigned long>(snapshot.dwords[4]),
                static_cast<unsigned long>(snapshot.dwords[5]));
            QueueVirtualATap(g_lastObservedUserIndex.load(),
                stick,
                g_lastObservedButtons.load(),
                0,
                "walking-retry");
        } else if (now > retryUntil || g_tapAttemptsForArm.load() >= 2) {
            g_retryAfterMs.store(0);
            g_retryUntilMs.store(0);
            Log("walking retry expired: locomotion=%s leftX=%d leftY=%d stick=%d d098=0x%08lx d09c=0x%08lx",
                ClassifyLocomotionState(snapshot),
                g_lastObservedLeftX.load(),
                g_lastObservedLeftY.load(),
                stick,
                static_cast<unsigned long>(snapshot.dwords[4]),
                static_cast<unsigned long>(snapshot.dwords[5]));
        }
        return;
    }

    if (!g_hookArmed.load()) {
        g_walkingSinceMs.store(0);
        return;
    }

    DWORD64 walkingSince = g_walkingSinceMs.load();
    if (walkingSince == 0) {
        g_walkingSinceMs.store(now);
        Log("walking candidate: locomotion=%s leftX=%d leftY=%d stick=%d d098=0x%08lx d09c=0x%08lx stableMs=%lu",
            ClassifyLocomotionState(snapshot),
            g_lastObservedLeftX.load(),
            g_lastObservedLeftY.load(),
            stick,
            static_cast<unsigned long>(snapshot.dwords[4]),
            static_cast<unsigned long>(snapshot.dwords[5]),
            g_settings.walkingStableMs);
        return;
    }

    if (now - walkingSince < g_settings.walkingStableMs) {
        return;
    }

    g_hookPending.store(true);
    Log("walking trigger ready: locomotion=%s leftX=%d leftY=%d stick=%d elapsedMs=%llu d098=0x%08lx d09c=0x%08lx",
        ClassifyLocomotionState(snapshot),
        g_lastObservedLeftX.load(),
        g_lastObservedLeftY.load(),
        stick,
        static_cast<unsigned long long>(now - walkingSince),
        static_cast<unsigned long>(snapshot.dwords[4]),
        static_cast<unsigned long>(snapshot.dwords[5]));
    QueueVirtualATap(g_lastObservedUserIndex.load(),
        stick,
        g_lastObservedButtons.load(),
        0,
        "walking-state");
    g_walkingSinceMs.store(0);
}

void PollAdaptiveTapHold() {
    const DWORD64 now = GetTickCount64();
    const DWORD64 until = g_aButtonUntilMs.load();
    const DWORD64 maxUntil = g_aButtonMaxUntilMs.load();
    if (until == 0 || maxUntil == 0 || now >= g_aButtonReleaseUntilMs.load()) {
        return;
    }

    if (now >= maxUntil) {
        bool expected = false;
        if (g_aButtonMaxLogged.compare_exchange_strong(expected, true)) {
            CharacterControlFocusSnapshot snapshot{};
            if (ReadCharacterControlFocusSnapshot(&snapshot)) {
                Log("adaptive tap max reached: locomotion=%s elapsedMs=%llu d098=0x%08lx d09c=0x%08lx",
                    ClassifyLocomotionState(snapshot),
                    static_cast<unsigned long long>(now - g_aButtonStartedMs.load()),
                    static_cast<unsigned long>(snapshot.dwords[4]),
                    static_cast<unsigned long>(snapshot.dwords[5]));
                if (IsWalkingForwardState(snapshot) && g_tapAttemptsForArm.load() < 2 &&
                    g_lastObservedLeftMagnitude.load() >= g_settings.forwardOn) {
                    g_retryAfterMs.store(now + g_settings.retryDelayMs);
                    g_retryUntilMs.store(now + g_settings.retryWindowMs);
                    Log("walking retry armed: afterMs=%lu windowMs=%lu leftX=%d leftY=%d stick=%d",
                        g_settings.retryDelayMs,
                        g_settings.retryWindowMs,
                        g_lastObservedLeftX.load(),
                        g_lastObservedLeftY.load(),
                        g_lastObservedLeftMagnitude.load());
                }
            } else {
                Log("adaptive tap max reached: elapsedMs=%llu ccc unavailable",
                    static_cast<unsigned long long>(now - g_aButtonStartedMs.load()));
            }
        }
        return;
    }

    CharacterControlFocusSnapshot snapshot{};
    if (ReadCharacterControlFocusSnapshot(&snapshot) && IsJogPromotionState(snapshot)) {
        g_aButtonUntilMs.store(now);
        g_aButtonReleaseUntilMs.store(now + g_settings.releaseDurationMs);
        g_aButtonMaxUntilMs.store(0);
        g_retryAfterMs.store(0);
        g_retryUntilMs.store(0);
        Log("adaptive tap release: locomotion=%s elapsedMs=%llu d098=0x%08lx d09c=0x%08lx",
            ClassifyLocomotionState(snapshot),
            static_cast<unsigned long long>(now - g_aButtonStartedMs.load()),
            static_cast<unsigned long>(snapshot.dwords[4]),
            static_cast<unsigned long>(snapshot.dwords[5]));
        return;
    }

    if (until < maxUntil) {
        g_aButtonUntilMs.store(maxUntil);
        g_aButtonReleaseUntilMs.store(maxUntil + g_settings.releaseDurationMs);
    }
}

DWORD WINAPI PollThread(LPVOID) {
    LoadSettings();
    Log("settings: enabled=%d diagnostics=%d sharedPlayerLogging=%d writeTrace=%d forwardOn=%d forwardOff=%d pollRateMs=%lu tapDurationMs=%lu tapMaxDurationMs=%lu releaseDurationMs=%lu retryDelayMs=%lu retryWindowMs=%lu triggerDelayMs=%lu walkingStableMs=%lu writeTraceMaxEvents=%lu",
        g_settings.enabled ? 1 : 0,
        g_settings.diagnostics ? 1 : 0,
        g_settings.sharedPlayerLogging ? 1 : 0,
        g_settings.writeTrace ? 1 : 0,
        g_settings.forwardOn,
        g_settings.forwardOff,
        g_settings.pollRateMs,
        g_settings.tapDurationMs,
        g_settings.tapMaxDurationMs,
        g_settings.releaseDurationMs,
        g_settings.retryDelayMs,
        g_settings.retryWindowMs,
        g_settings.triggerDelayMs,
        g_settings.walkingStableMs,
        g_settings.writeTraceMaxEvents);

    void* importedXInput = nullptr;
    const bool xinputImportHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "XInputGetState",
        reinterpret_cast<void*>(&HookedXInputGetState),
        &importedXInput);
    if (xinputImportHook && importedXInput != nullptr) {
        g_originalXInputGetState = reinterpret_cast<XInputGetStateFn>(importedXInput);
    }

    void* importedXInputOrdinal = nullptr;
    const bool xinputOrdinalHook = HookModuleOrdinalImport(
        GetModuleHandleA(nullptr),
        "XINPUT1_4.dll",
        2,
        reinterpret_cast<void*>(&HookedXInputGetState),
        &importedXInputOrdinal);
    if (xinputOrdinalHook && importedXInputOrdinal != nullptr) {
        g_originalXInputGetState = reinterpret_cast<XInputGetStateFn>(importedXInputOrdinal);
    }

    void* importedXInputSetState = nullptr;
    const bool xinputSetHook = HookModuleOrdinalImport(
        GetModuleHandleA(nullptr),
        "XINPUT1_4.dll",
        3,
        reinterpret_cast<void*>(&HookedXInputSetState),
        &importedXInputSetState);
    if (xinputSetHook && importedXInputSetState != nullptr) {
        g_originalXInputSetState = reinterpret_cast<XInputSetStateFn>(importedXInputSetState);
    }

    void* importedXInputCapabilities = nullptr;
    const bool xinputCapabilitiesHook = HookModuleOrdinalImport(
        GetModuleHandleA(nullptr),
        "XINPUT1_4.dll",
        4,
        reinterpret_cast<void*>(&HookedXInputGetCapabilities),
        &importedXInputCapabilities);
    if (xinputCapabilitiesHook && importedXInputCapabilities != nullptr) {
        g_originalXInputGetCapabilities =
            reinterpret_cast<XInputGetCapabilitiesFn>(importedXInputCapabilities);
    }

    void* importedXInputKeystrokeName = nullptr;
    const bool xinputKeystrokeNameHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "XInputGetKeystroke",
        reinterpret_cast<void*>(&HookedXInputGetKeystroke),
        &importedXInputKeystrokeName);
    if (xinputKeystrokeNameHook && importedXInputKeystrokeName != nullptr) {
        g_originalXInputGetKeystroke =
            reinterpret_cast<XInputGetKeystrokeFn>(importedXInputKeystrokeName);
    }

    void* importedXInputKeystrokeOrdinal = nullptr;
    const bool xinputKeystrokeOrdinalHook = HookModuleOrdinalImport(
        GetModuleHandleA(nullptr),
        "XINPUT1_4.dll",
        8,
        reinterpret_cast<void*>(&HookedXInputGetKeystroke),
        &importedXInputKeystrokeOrdinal);
    if (xinputKeystrokeOrdinalHook && importedXInputKeystrokeOrdinal != nullptr) {
        g_originalXInputGetKeystroke =
            reinterpret_cast<XInputGetKeystrokeFn>(importedXInputKeystrokeOrdinal);
    }

    void* importedGetProcAddress = nullptr;
    const bool getProcHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "GetProcAddress",
        reinterpret_cast<void*>(&HookedGetProcAddress),
        &importedGetProcAddress);
    if (getProcHook && importedGetProcAddress != nullptr) {
        g_originalGetProcAddress = reinterpret_cast<GetProcAddressFn>(importedGetProcAddress);
    }

    void* importedLoadLibraryA = nullptr;
    const bool loadLibraryAHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "LoadLibraryA",
        reinterpret_cast<void*>(&HookedLoadLibraryA),
        &importedLoadLibraryA);
    if (loadLibraryAHook && importedLoadLibraryA != nullptr) {
        g_originalLoadLibraryA = reinterpret_cast<LoadLibraryAFn>(importedLoadLibraryA);
    }

    void* importedLoadLibraryW = nullptr;
    const bool loadLibraryWHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "LoadLibraryW",
        reinterpret_cast<void*>(&HookedLoadLibraryW),
        &importedLoadLibraryW);
    if (loadLibraryWHook && importedLoadLibraryW != nullptr) {
        g_originalLoadLibraryW = reinterpret_cast<LoadLibraryWFn>(importedLoadLibraryW);
    }

    void* importedLoadLibraryExA = nullptr;
    const bool loadLibraryExAHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "LoadLibraryExA",
        reinterpret_cast<void*>(&HookedLoadLibraryExA),
        &importedLoadLibraryExA);
    if (loadLibraryExAHook && importedLoadLibraryExA != nullptr) {
        g_originalLoadLibraryExA = reinterpret_cast<LoadLibraryExAFn>(importedLoadLibraryExA);
    }

    void* importedLoadLibraryExW = nullptr;
    const bool loadLibraryExWHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "LoadLibraryExW",
        reinterpret_cast<void*>(&HookedLoadLibraryExW),
        &importedLoadLibraryExW);
    if (loadLibraryExWHook && importedLoadLibraryExW != nullptr) {
        g_originalLoadLibraryExW = reinterpret_cast<LoadLibraryExWFn>(importedLoadLibraryExW);
    }

    void* importedRawData = nullptr;
    const bool rawDataHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "GetRawInputData",
        reinterpret_cast<void*>(&HookedGetRawInputData),
        &importedRawData);
    if (rawDataHook && importedRawData != nullptr) {
        g_originalGetRawInputData = reinterpret_cast<GetRawInputDataFn>(importedRawData);
    }

    void* importedRawDeviceInfo = nullptr;
    const bool rawDeviceInfoHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "GetRawInputDeviceInfoW",
        reinterpret_cast<void*>(&HookedGetRawInputDeviceInfoW),
        &importedRawDeviceInfo);
    if (rawDeviceInfoHook && importedRawDeviceInfo != nullptr) {
        g_originalGetRawInputDeviceInfoW =
            reinterpret_cast<GetRawInputDeviceInfoWFn>(importedRawDeviceInfo);
    }

    void* importedRawDeviceList = nullptr;
    const bool rawDeviceListHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "GetRawInputDeviceList",
        reinterpret_cast<void*>(&HookedGetRawInputDeviceList),
        &importedRawDeviceList);
    if (rawDeviceListHook && importedRawDeviceList != nullptr) {
        g_originalGetRawInputDeviceList =
            reinterpret_cast<GetRawInputDeviceListFn>(importedRawDeviceList);
    }

    void* importedRawRegister = nullptr;
    const bool rawRegisterHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "RegisterRawInputDevices",
        reinterpret_cast<void*>(&HookedRegisterRawInputDevices),
        &importedRawRegister);
    if (rawRegisterHook && importedRawRegister != nullptr) {
        g_originalRegisterRawInputDevices =
            reinterpret_cast<RegisterRawInputDevicesFn>(importedRawRegister);
    }

    void* importedRawRegistered = nullptr;
    const bool rawRegisteredHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "GetRegisteredRawInputDevices",
        reinterpret_cast<void*>(&HookedGetRegisteredRawInputDevices),
        &importedRawRegistered);
    if (rawRegisteredHook && importedRawRegistered != nullptr) {
        g_originalGetRegisteredRawInputDevices =
            reinterpret_cast<GetRegisteredRawInputDevicesFn>(importedRawRegistered);
    }

    void* importedHidAttributes = nullptr;
    const bool hidAttributesHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "HidD_GetAttributes",
        reinterpret_cast<void*>(&HookedHidD_GetAttributes),
        &importedHidAttributes);
    if (hidAttributesHook && importedHidAttributes != nullptr) {
        g_originalHidD_GetAttributes = reinterpret_cast<HidD_GetAttributesFn>(importedHidAttributes);
    }

    void* importedHidPreparsed = nullptr;
    const bool hidPreparsedHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "HidD_GetPreparsedData",
        reinterpret_cast<void*>(&HookedHidD_GetPreparsedData),
        &importedHidPreparsed);
    if (hidPreparsedHook && importedHidPreparsed != nullptr) {
        g_originalHidD_GetPreparsedData =
            reinterpret_cast<HidD_GetPreparsedDataFn>(importedHidPreparsed);
    }

    void* importedHidFreePreparsed = nullptr;
    const bool hidFreePreparsedHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "HidD_FreePreparsedData",
        reinterpret_cast<void*>(&HookedHidD_FreePreparsedData),
        &importedHidFreePreparsed);
    if (hidFreePreparsedHook && importedHidFreePreparsed != nullptr) {
        g_originalHidD_FreePreparsedData =
            reinterpret_cast<HidD_FreePreparsedDataFn>(importedHidFreePreparsed);
    }

    void* importedHidGetFeature = nullptr;
    const bool hidGetFeatureHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "HidD_GetFeature",
        reinterpret_cast<void*>(&HookedHidD_GetFeature),
        &importedHidGetFeature);
    if (hidGetFeatureHook && importedHidGetFeature != nullptr) {
        g_originalHidD_GetFeature = reinterpret_cast<HidD_GetFeatureFn>(importedHidGetFeature);
    }

    void* importedHidSetFeature = nullptr;
    const bool hidSetFeatureHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "HidD_SetFeature",
        reinterpret_cast<void*>(&HookedHidD_SetFeature),
        &importedHidSetFeature);
    if (hidSetFeatureHook && importedHidSetFeature != nullptr) {
        g_originalHidD_SetFeature = reinterpret_cast<HidD_SetFeatureFn>(importedHidSetFeature);
    }

    void* importedHidGetCaps = nullptr;
    const bool hidGetCapsHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "HidP_GetCaps",
        reinterpret_cast<void*>(&HookedHidP_GetCaps),
        &importedHidGetCaps);
    if (hidGetCapsHook && importedHidGetCaps != nullptr) {
        g_originalHidP_GetCaps = reinterpret_cast<HidP_GetCapsFn>(importedHidGetCaps);
    }

    void* importedHidGetValueCaps = nullptr;
    const bool hidGetValueCapsHook = HookModuleImport(
        GetModuleHandleA(nullptr),
        "HidP_GetValueCaps",
        reinterpret_cast<void*>(&HookedHidP_GetValueCaps),
        &importedHidGetValueCaps);
    if (hidGetValueCapsHook && importedHidGetValueCaps != nullptr) {
        g_originalHidP_GetValueCaps =
            reinterpret_cast<HidP_GetValueCapsFn>(importedHidGetValueCaps);
    }

    Log("hooks: xinputName=%s xinputStateOrd2=%s xinputSetOrd3=%s xinputCapsOrd4=%s xinputKeystrokeName=%s xinputKeystrokeOrd8=%s getProc=%s",
        xinputImportHook ? "installed" : "not installed",
        xinputOrdinalHook ? "installed" : "not installed",
        xinputSetHook ? "installed" : "not installed",
        xinputCapabilitiesHook ? "installed" : "not installed",
        xinputKeystrokeNameHook ? "installed" : "not installed",
        xinputKeystrokeOrdinalHook ? "installed" : "not installed",
        getProcHook ? "installed" : "not installed");
    Log("hooks: loadLibraryA=%s loadLibraryW=%s loadLibraryExA=%s loadLibraryExW=%s",
        loadLibraryAHook ? "installed" : "not installed",
        loadLibraryWHook ? "installed" : "not installed",
        loadLibraryExAHook ? "installed" : "not installed",
        loadLibraryExWHook ? "installed" : "not installed");
    Log("hooks: rawData=%s rawDeviceInfo=%s rawDeviceList=%s rawRegister=%s rawRegistered=%s",
        rawDataHook ? "installed" : "not installed",
        rawDeviceInfoHook ? "installed" : "not installed",
        rawDeviceListHook ? "installed" : "not installed",
        rawRegisterHook ? "installed" : "not installed",
        rawRegisteredHook ? "installed" : "not installed");
    Log("hooks: hidAttributes=%s hidPreparsed=%s hidFreePreparsed=%s hidGetFeature=%s hidSetFeature=%s hidGetCaps=%s hidGetValueCaps=%s",
        hidAttributesHook ? "installed" : "not installed",
        hidPreparsedHook ? "installed" : "not installed",
        hidFreePreparsedHook ? "installed" : "not installed",
        hidGetFeatureHook ? "installed" : "not installed",
        hidSetFeatureHook ? "installed" : "not installed",
        hidGetCapsHook ? "installed" : "not installed",
        hidGetValueCapsHook ? "installed" : "not installed");

    DWORD64 lastStatsMs = GetTickCount64();
    DWORD64 lastTargetedMs = GetTickCount64();
    DWORD64 lastWatchMs = GetTickCount64();
    while (g_running.load()) {
        Sleep(g_settings.pollRateMs);

        const DWORD64 now = GetTickCount64();
        PollAdaptiveTapHold();
        PollWalkingJogTrigger();
        EnsureWriteTraceInstalled();
        FlushWriteTraceEvents();
        if (now - lastWatchMs >= 50) {
            lastWatchMs = now;
            LogCharacterControlFocus("watch", false);
        }

        if (now - lastTargetedMs >= 1000) {
            lastTargetedMs = now;
            Log("sample: input leftX=%d leftY=%d stick=%d buttons=0x%04x armed=%d pending=%d",
                g_lastObservedLeftX.load(),
                g_lastObservedLeftY.load(),
                g_lastObservedLeftMagnitude.load(),
                g_lastObservedButtons.load(),
                g_hookArmed.load() ? 1 : 0,
                g_hookPending.load() ? 1 : 0);
            LogCdCoreSnapshot("sample", false);
            LogCharacterControlFocus("sample", true);
        }

        if (now - lastStatsMs >= 5000) {
            lastStatsMs = now;
            VelocitySnapshot snap{};
            ReadVelocitySnapshot(&snap);
            if (snap.valid) {
                Log("player: base=0x%p vel={x=%.4f z=%.4f y=%.4f w=%.4f} ageMs=%lu owner=%s velHook=%ld posHook=%ld",
                    reinterpret_cast<void*>(snap.playerBase),
                    snap.x,
                    snap.z,
                    snap.y,
                    snap.w,
                    snap.ageMs,
                    g_sharedPlayer != nullptr ? g_sharedPlayer->hookOwnerName : "",
                    g_sharedPlayer != nullptr ? g_sharedPlayer->velHookInstalled : 0,
                    g_sharedPlayer != nullptr ? g_sharedPlayer->posHookInstalled : 0);
            } else if (g_settings.sharedPlayerLogging) {
                Log("player: unavailable base=0x%p ageMs=%lu sharedMap=%s",
                    reinterpret_cast<void*>(snap.playerBase),
                    snap.ageMs,
                    g_sharedPlayer == nullptr ? "closed" : "open");
            }

            LogCdCoreSnapshot("periodic", false);
            LogCharacterControlFocus("periodic", true);

            Log("stats: xinputGetState=%lu thresholdCrossings=%lu injected=%lu xinputKeystroke=%lu injectedKeys=%lu leftX=%d leftY=%d stick=%d buttons=0x%04x armed=%d pending=%d sharedReads=%lu sharedValid=%lu cdCoreReads=%lu cdCoreControlled=%lu cccReads=%lu cccValid=%lu cccChanges=%lu writeTrace=%lu writeTraceDropped=%lu xinputSetState=%lu xinputCaps=%lu rawData=%lu rawInput=%lu rawHeader=%lu rawInfo=%lu rawList=%lu rawRegister=%lu rawRegistered=%lu hidAttr=%lu hidPrep=%lu hidFreePrep=%lu hidGetFeature=%lu hidSetFeature=%lu hidCaps=%lu hidValueCaps=%lu loadLibrary=%lu getProc=%lu",
                g_hookCalls.load(),
                g_thresholdCrossings.load(),
                g_injectedCalls.load(),
                g_xinputKeystrokeCalls.load(),
                g_injectedKeystrokes.load(),
                g_lastObservedLeftX.load(),
                g_lastObservedLeftY.load(),
                g_lastObservedLeftMagnitude.load(),
                g_lastObservedButtons.load(),
                g_hookArmed.load() ? 1 : 0,
                g_hookPending.load() ? 1 : 0,
                g_sharedPlayerReads.load(),
                g_sharedPlayerValidReads.load(),
                g_cdCoreReads.load(),
                g_cdCoreControlledReads.load(),
                g_characterControlReads.load(),
                g_characterControlValidReads.load(),
                g_characterControlStateChanges.load(),
                g_writeTraceEvents.load(),
                g_writeTraceDropped.load(),
                g_xinputSetStateCalls.load(),
                g_xinputCapabilitiesCalls.load(),
                g_rawInputDataCalls.load(),
                g_rawInputInputCalls.load(),
                g_rawInputHeaderCalls.load(),
                g_rawInputDeviceInfoCalls.load(),
                g_rawInputDeviceListCalls.load(),
                g_rawInputRegisterCalls.load(),
                g_rawInputRegisteredCalls.load(),
                g_hidAttributesCalls.load(),
                g_hidPreparsedCalls.load(),
                g_hidFreePreparsedCalls.load(),
                g_hidGetFeatureCalls.load(),
                g_hidSetFeatureCalls.load(),
                g_hidGetCapsCalls.load(),
                g_hidGetValueCapsCalls.load(),
                g_loadLibraryCalls.load(),
                g_getProcAddressCalls.load());
        }
    }

    Log("poll thread exiting");
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        InitializePaths();

        char hostPath[MAX_PATH]{};
        GetModuleFileNameA(nullptr, hostPath, MAX_PATH);
        const std::string hostName = BaseName(hostPath);
        g_isTargetProcess = _stricmp(hostName.c_str(), "CrimsonDesert.exe") == 0;

        Log("CrimsonDesertAnalogMovement loaded: pid=%lu host=%s",
            GetCurrentProcessId(),
            hostPath);

        if (!g_isTargetProcess) {
            Log("skipping non-target process");
            return TRUE;
        }

        bool expected = false;
        if (!g_threadStarted.compare_exchange_strong(expected, true)) {
            Log("poll thread already started");
            return TRUE;
        }

        HANDLE thread = CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        } else {
            g_threadStarted.store(false);
            Log("failed to create poll thread");
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        g_running.store(false);
        ShutdownWriteTrace();
        if (g_sharedPlayer != nullptr) {
            UnmapViewOfFile(g_sharedPlayer);
            g_sharedPlayer = nullptr;
        }
        if (g_sharedPlayerMap != nullptr) {
            CloseHandle(g_sharedPlayerMap);
            g_sharedPlayerMap = nullptr;
        }
        Log("CrimsonDesertAnalogMovement unloaded");
    }
    return TRUE;
}
