#include "plugin.h"
#include "CCamera.h"
#include "CDraw.h"
#include "CClock.h"
#include "CWeather.h"
#include "CCam.h"
#include "CHud.h"
#include "CPad.h"
#include "CTimer.h"
#include "CVector.h"
#include "CFont.h"
#include "CRunningScript.h"

#include <Windows.h>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <cctype>

// sapp_amazing_screenshot.cpp
// GTA San Andreas 1.0 US / Plugin-SDK.
// Independent screenshot-camera plugin with INI-configurable controls.
// The camera is driven through GTA's CCamera / CCam data and fixed-camera API.

namespace
{
    constexpr float DEFAULT_FOV = 70.0f;
    constexpr float MIN_PITCH = -1.50f;
    constexpr float MAX_PITCH = 1.50f;
    constexpr float MAX_FRAME_DELTA = 0.05f;

    struct PluginSettings
    {
        bool freezeTime;
        bool hideHud;
        bool freezeEffectsOnStart;
        bool showHelpOnStart;
        float minFov;
        float maxFov;
        float zoomStep;
        float mouseSensitivity;
        float baseMoveSpeed;
        float fastMoveMultiplier;
        float slowMoveMultiplier;
        int keyModeToggle;
        int keyWideScreen;
        int keyZoomIn;
        int keyZoomOut;
        int keyResetZoom;
        int keyShowHelp;
        int keyFreezeEffects;
        int keyCapturePhoto;
        int keyAscend;
        int keyDescend;
        int keyForward;
        int keyBackward;
        int keyLeft;
        int keyRight;
        int keyMoveFast;
        int keyMoveSlow;
        float helpFontSize;
        int hoursPerPress;
        int keyHourDown;
        int keyHourUp;
    };

    PluginSettings g_settings = {
        true, true, true, true,
        10.0f, 150.0f, 0.5f, 0.0026f, 7.0f, 3.0f, 0.22f,
        'V', VK_TAB, VK_OEM_4, VK_OEM_6, 'P', VK_F1, VK_F2, VK_F12,
        VK_SPACE, 'C', 'W', 'S', 'A', 'D', VK_SHIFT, VK_CONTROL, 0.45f,
        1, VK_OEM_MINUS, VK_OEM_PLUS
    };

    // GTA San Andreas 1.0 US (32-bit) native CHud draw entry points.
    // CHud::Draw()       = 0x58FAE0
    // CHud::DrawAfterFade() = 0x58D490
    // Returning immediately from these two top-level render functions hides
    // every native HUD layer, including elements not covered by HUD flags.
    constexpr std::uintptr_t HUD_DRAW_ADDRESS = 0x58FAE0;
    constexpr std::uintptr_t HUD_DRAW_AFTER_FADE_ADDRESS = 0x58D490;
    // GTA SA 1.0 US update entry points for world effects. FxManager_c::Update
    // is __thiscall with two stack arguments, so its early return must clean 8 bytes.
    // CCoronas::Update is __cdecl with no stack arguments.
    constexpr std::uintptr_t FX_MANAGER_UPDATE_ADDRESS = 0x4A9A80;
    constexpr std::uintptr_t CORONAS_UPDATE_ADDRESS = 0x6FADF0;
    constexpr BYTE NATIVE_RETURN_OPCODE = 0xC3; // x86 RET
    constexpr BYTE FX_MANAGER_RETURN_WITH_STACK_CLEANUP[3] = { 0xC2, 0x08, 0x00 }; // RET 8

    bool g_bScreenshotMode = false;
    bool g_bFreeCamera = false;
    bool g_bWideScreenCurrent = false;
    bool g_bPreviousKeys[256] = {};
    bool g_bPreviousMappedCharacters[256] = {};
    int g_selectedWeatherPreset = -1;

    CVector g_cameraPosition(0.0f, 0.0f, 0.0f);
    CVector g_cameraForward(0.0f, 1.0f, 0.0f);
    float g_cameraYaw = 0.0f;
    float g_cameraPitch = 0.0f;
    float g_cameraFov = DEFAULT_FOV;
    float g_modeInitialFov = DEFAULT_FOV;
    unsigned int g_lastUpdateTick = 0;
    bool g_bControlHelpVisible = false;
    bool g_bPhotoCaptureRequested = false;
    bool g_bSceneEffectsFrozen = false;
    bool g_bSceneEffectsPatchesApplied = false;

    BYTE g_savedFxManagerUpdateBytes[3] = {};
    BYTE g_savedCoronasUpdateByte = 0;
    bool g_bZoomKeyActive = false;

    bool g_bNativeHudDrawingSuppressed = false;
    BYTE g_savedHudDrawOpcode = 0;
    BYTE g_savedHudDrawAfterFadeOpcode = 0;

    float g_savedTimeScale = 1.0f;
    bool g_savedHudVisible = true;
    bool g_savedRadarHidden = false;
    bool g_savedAreaNameHidden = false;
    bool g_savedVehicleNameHidden = false;
    bool g_savedClockVisible = true;
    bool g_saved3dMarkers = true;
    bool g_savedWideScreen = false;
    unsigned short g_savedPlayerControlState = 0;
    float g_savedCameraFov[3] = { DEFAULT_FOV, DEFAULT_FOV, DEFAULT_FOV };
    float g_savedDrawFov = DEFAULT_FOV;

    const char* GetConfigurationPath()
    {
        static char path[MAX_PATH] = {};
        if (path[0] == '\0')
        {
            // Resolve the module containing this plugin code, not gta_sa.exe.
            // This keeps AmazingSnapshot.ini beside the ASI even when the game
            // executable and plugin are loaded from different directories.
            HMODULE pluginModule = nullptr;
            const DWORD moduleFlags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
            // Resolve from an address inside executable code first. If a host
            // toolchain places the helper in a separate section unexpectedly,
            // retry with the settings object's data address.
            if (!GetModuleHandleExA(moduleFlags,
                                    reinterpret_cast<LPCSTR>(&GetConfigurationPath),
                                    &pluginModule))
            {
                GetModuleHandleExA(moduleFlags, reinterpret_cast<LPCSTR>(&g_settings), &pluginModule);
            }

            char modulePath[MAX_PATH] = {};
            const DWORD length = pluginModule ? GetModuleFileNameA(pluginModule, modulePath, MAX_PATH) : 0;
            if (length == 0 || length >= MAX_PATH)
            {
                std::strncpy(path, "AmazingSnapshot.ini", MAX_PATH - 1);
                path[MAX_PATH - 1] = '\0';
            }
            else
            {
                char* lastSlash = std::strrchr(modulePath, '\\');
                char* lastForwardSlash = std::strrchr(modulePath, '/');
                if (!lastSlash || (lastForwardSlash && lastForwardSlash > lastSlash))
                    lastSlash = lastForwardSlash;
                if (lastSlash)
                    lastSlash[1] = '\0';
                else
                    modulePath[0] = '\0';
                std::strncpy(path, modulePath, MAX_PATH - 1);
                path[MAX_PATH - 1] = '\0';
                const std::size_t used = std::strlen(path);
                if (used < MAX_PATH - 1)
                {
                    std::strncpy(path + used, "AmazingSnapshot.ini", MAX_PATH - used - 1);
                    path[MAX_PATH - 1] = '\0';
                }
            }
        }
        return path;
    }

    float ReadIniFloat(const char* section, const char* key, float defaultValue)
    {
        char defaultText[48] = {};
        char valueText[64] = {};
        std::snprintf(defaultText, sizeof(defaultText), "%.6f", defaultValue);
        GetPrivateProfileStringA(section, key, defaultText, valueText,
                                 static_cast<DWORD>(sizeof(valueText)), GetConfigurationPath());
        char* end = nullptr;
        const float value = std::strtof(valueText, &end);
        if (end == valueText || !std::isfinite(value))
            return defaultValue;

        // Reject values with trailing junk (e.g. "0.5abc") instead of silently
        // accepting a prefix and making an INI edit appear to be ignored.
        while (*end && std::isspace(static_cast<unsigned char>(*end)))
            ++end;
        return (*end == '\0') ? value : defaultValue;
    }

    bool ReadIniBool(const char* section, const char* key, bool defaultValue)
    {
        char defaultText[8] = {};
        char valueText[32] = {};
        std::snprintf(defaultText, sizeof(defaultText), "%d", defaultValue ? 1 : 0);
        GetPrivateProfileStringA(section, key, defaultText, valueText,
                                 static_cast<DWORD>(sizeof(valueText)), GetConfigurationPath());
        std::string value(valueText);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
        for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
        if (value == "0" || value == "false" || value == "no" || value == "off") return false;
        return defaultValue;
    }

    int ParseVirtualKey(const char* text, int fallback)
    {
        if (!text)
            return fallback;
        std::string key(text);
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front()))) key.erase(key.begin());
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
        for (char& ch : key) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

        if (key.empty()) return fallback;
        if (key.size() == 1)
        {
            const unsigned char ch = static_cast<unsigned char>(key[0]);
            if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) return ch;
        }
        if (key.size() >= 2 && key[0] == 'F')
        {
            char* end = nullptr;
            const long number = std::strtol(key.c_str() + 1, &end, 10);
            if (end != key.c_str() + 1 && *end == '\0' && number >= 1 && number <= 24)
                return VK_F1 + static_cast<int>(number) - 1;
        }

        if (key == "SPACE") return VK_SPACE;
        if (key == "TAB") return VK_TAB;
        if (key == "ESC" || key == "ESCAPE") return VK_ESCAPE;
        if (key == "SHIFT") return VK_SHIFT;
        if (key == "CTRL" || key == "CONTROL") return VK_CONTROL;
        if (key == "ALT") return VK_MENU;
        if (key == "PAGEUP" || key == "PGUP") return VK_PRIOR;
        if (key == "PAGEDOWN" || key == "PGDN") return VK_NEXT;
        if (key == "UP") return VK_UP;
        if (key == "DOWN") return VK_DOWN;
        if (key == "LEFT") return VK_LEFT;
        if (key == "RIGHT") return VK_RIGHT;
        if (key == "OEM_4" || key == "LBRACKET" || key == "[") return VK_OEM_4;
        if (key == "OEM_6" || key == "RBRACKET" || key == "]") return VK_OEM_6;
        if (key == "OEM_COMMA" || key == "COMMA" || key == ",") return VK_OEM_COMMA;
        if (key == "OEM_PERIOD" || key == "PERIOD" || key == ".") return VK_OEM_PERIOD;
        if (key == "OEM_102" || key == "ISO_LESS_GREATER" || key == "<>") return VK_OEM_102;
        if (key == "OEM_MINUS" || key == "MINUS" || key == "-") return VK_OEM_MINUS;
        if (key == "OEM_PLUS" || key == "PLUS" || key == "+") return VK_OEM_PLUS;
        if (key == "NUM_PLUS" || key == "ADD") return VK_ADD;
        if (key == "NUM_MINUS" || key == "SUBTRACT") return VK_SUBTRACT;
        if (key == "ENTER") return VK_RETURN;
        if (key == "BACKSPACE") return VK_BACK;
        return fallback;
    }

    int ReadIniKey(const char* name, const char* defaultValue, int fallback)
    {
        char value[64] = {};
        GetPrivateProfileStringA("Keys", name, defaultValue, value,
                                 static_cast<DWORD>(sizeof(value)), GetConfigurationPath());
        return ParseVirtualKey(value, fallback);
    }

    void LoadConfiguration()
    {
        g_settings.freezeTime = ReadIniBool("General", "FreezeTime", true);
        g_settings.hideHud = ReadIniBool("General", "HideHUD", true);
        g_settings.freezeEffectsOnStart = ReadIniBool("General", "FreezeEffectsOnStart", true);
        g_settings.showHelpOnStart = ReadIniBool("General", "ShowHelpOnStart", true);
        g_settings.helpFontSize = ReadIniFloat("Interface", "FontSize", 0.45f);
        g_settings.minFov = ReadIniFloat("Camera", "MinimumFOV", 10.0f);
        g_settings.maxFov = ReadIniFloat("Camera", "MaximumFOV", 150.0f);
        // ZoomStep mirrors the original script: change FOV by 0.5 degrees per
        // process tick while [ or ] is held. Accept old ZoomSpeed (degrees/sec)
        // from earlier INI versions as a compatibility fallback.
        g_settings.zoomStep = ReadIniFloat("Camera", "ZoomStep", -1.0f);
        if (g_settings.zoomStep < 0.0f)
            g_settings.zoomStep = ReadIniFloat("Camera", "ZoomSpeed", 30.0f) / 60.0f;
        g_settings.mouseSensitivity = ReadIniFloat("Camera", "MouseSensitivity", 0.0026f);
        g_settings.baseMoveSpeed = ReadIniFloat("Camera", "MoveSpeed", 7.0f);
        g_settings.fastMoveMultiplier = ReadIniFloat("Camera", "FastMoveMultiplier", 3.0f);
        g_settings.slowMoveMultiplier = ReadIniFloat("Camera", "SlowMoveMultiplier", 0.22f);
        g_settings.hoursPerPress = static_cast<int>(ReadIniFloat("Environment", "HoursPerPress", 1.0f));

        // FontSize is a direct GTA CFont scale. Default: 0.45.
        // Clamp out-of-range values rather than silently falling back to default.
        g_settings.helpFontSize = std::max(0.18f, std::min(0.75f, g_settings.helpFontSize));
        if (g_settings.minFov < 1.0f || g_settings.minFov > 60.0f) g_settings.minFov = 10.0f;
        if (g_settings.maxFov < 40.0f || g_settings.maxFov > 179.0f) g_settings.maxFov = 150.0f;
        if (g_settings.minFov >= g_settings.maxFov) { g_settings.minFov = 10.0f; g_settings.maxFov = 150.0f; }
        if (g_settings.zoomStep < 0.05f || g_settings.zoomStep > 5.0f) g_settings.zoomStep = 0.5f;
        if (g_settings.mouseSensitivity < 0.0001f || g_settings.mouseSensitivity > 0.02f) g_settings.mouseSensitivity = 0.0026f;
        if (g_settings.baseMoveSpeed < 0.1f || g_settings.baseMoveSpeed > 100.0f) g_settings.baseMoveSpeed = 7.0f;
        if (g_settings.fastMoveMultiplier < 1.0f || g_settings.fastMoveMultiplier > 20.0f) g_settings.fastMoveMultiplier = 3.0f;
        if (g_settings.slowMoveMultiplier < 0.01f || g_settings.slowMoveMultiplier > 1.0f) g_settings.slowMoveMultiplier = 0.22f;
        if (g_settings.hoursPerPress < 1 || g_settings.hoursPerPress > 12) g_settings.hoursPerPress = 1;

        g_settings.keyModeToggle = ReadIniKey("ModeToggle", "V", 'V');
        g_settings.keyWideScreen = ReadIniKey("Widescreen", "TAB", VK_TAB);
        g_settings.keyZoomIn = ReadIniKey("ZoomIn", "OEM_4", VK_OEM_4);
        g_settings.keyZoomOut = ReadIniKey("ZoomOut", "OEM_6", VK_OEM_6);
        g_settings.keyResetZoom = ReadIniKey("ResetZoom", "P", 'P');
        g_settings.keyShowHelp = ReadIniKey("ToggleHelp", "F1", VK_F1);
        g_settings.keyFreezeEffects = ReadIniKey("ToggleEffectFreeze", "F2", VK_F2);
        g_settings.keyCapturePhoto = ReadIniKey("TakePhoto", "F12", VK_F12);
        g_settings.keyAscend = ReadIniKey("Ascend", "SPACE", VK_SPACE);
        g_settings.keyDescend = ReadIniKey("Descend", "C", 'C');
        g_settings.keyForward = ReadIniKey("MoveForward", "W", 'W');
        g_settings.keyBackward = ReadIniKey("MoveBackward", "S", 'S');
        g_settings.keyLeft = ReadIniKey("MoveLeft", "A", 'A');
        g_settings.keyRight = ReadIniKey("MoveRight", "D", 'D');
        g_settings.keyMoveFast = ReadIniKey("MoveFast", "SHIFT", VK_SHIFT);
        g_settings.keyMoveSlow = ReadIniKey("MoveSlow", "CTRL", VK_CONTROL);
        g_settings.keyHourDown = ReadIniKey("TimeHourDecrease", "OEM_MINUS", VK_OEM_MINUS);
        g_settings.keyHourUp = ReadIniKey("TimeHourIncrease", "OEM_PLUS", VK_OEM_PLUS);
    }

    std::string GetVirtualKeyLabel(int key)
    {
        if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
            return std::string(1, static_cast<char>(key));
        if (key >= VK_F1 && key <= VK_F1 + 23)
            return "F" + std::to_string(key - VK_F1 + 1);
        switch (key)
        {
        case VK_SPACE: return "Space";
        case VK_TAB: return "Tab";
        case VK_ESCAPE: return "Esc";
        case VK_SHIFT: return "Shift";
        case VK_CONTROL: return "Ctrl";
        case VK_MENU: return "Alt";
        case VK_PRIOR: return "Page Up";
        case VK_NEXT: return "Page Down";
        case VK_UP: return "Up";
        case VK_DOWN: return "Down";
        case VK_LEFT: return "Left";
        case VK_RIGHT: return "Right";
        case VK_OEM_4: return "[";
        case VK_OEM_6: return "]";
        case VK_OEM_COMMA: return ",";
        case VK_OEM_PERIOD: return ".";
        case VK_OEM_102: return "<>";
        case VK_OEM_MINUS: return "-";
        case VK_OEM_PLUS: return "+";
        case VK_ADD: return "Num +";
        case VK_SUBTRACT: return "Num -";
        default: return "VK" + std::to_string(key);
        }
    }

    bool IsKeyDown(int key)
    {
        return (GetAsyncKeyState(key) & 0x8000) != 0;
    }

    bool WasKeyPressed(int key)
    {
        const bool down = IsKeyDown(key);
        const bool pressed = down && !g_bPreviousKeys[key];
        g_bPreviousKeys[key] = down;
        return pressed;
    }

    // Use the active Windows keyboard layout for symbol bindings such as [, ], <, > and +.
    // OEM virtual-key constants alone are unreliable on non-US layouts because the same
    // printed symbol may live on a different physical key or require Shift.
    bool TryMapCharacter(char character, int& virtualKey, int& modifiers)
    {
        const SHORT mapped = VkKeyScanExA(character, GetKeyboardLayout(0));
        if (mapped == -1)
            return false;
        virtualKey = static_cast<unsigned char>(LOBYTE(mapped));
        modifiers = static_cast<unsigned char>(HIBYTE(mapped));
        return virtualKey > 0 && virtualKey < 256;
    }

    bool IsMappedCharacterDown(char character)
    {
        int virtualKey = 0;
        int modifiers = 0;
        if (!TryMapCharacter(character, virtualKey, modifiers) || !IsKeyDown(virtualKey))
            return false;

        const bool shift = IsKeyDown(VK_SHIFT);
        const bool control = IsKeyDown(VK_CONTROL);
        const bool alt = IsKeyDown(VK_MENU);
        if (((modifiers & 1) != 0) != shift) return false;
        if (((modifiers & 2) != 0) != control) return false;
        if (((modifiers & 4) != 0) != alt) return false;
        return true;
    }

    bool WasMappedCharacterPressed(char character)
    {
        const unsigned char index = static_cast<unsigned char>(character);
        const bool down = IsMappedCharacterDown(character);
        const bool pressed = down && !g_bPreviousMappedCharacters[index];
        g_bPreviousMappedCharacters[index] = down;
        return pressed;
    }

    float ClampFloat(float value, float minimum, float maximum)
    {
        return std::max(minimum, std::min(maximum, value));
    }

    CVector GetCameraForwardFromActiveCam()
    {
        if (TheCamera.m_nActiveCam < 3)
        {
            const CVector direction = TheCamera.m_aCams[TheCamera.m_nActiveCam].m_vecFront;
            const float length = std::sqrt(direction.x * direction.x +
                                           direction.y * direction.y +
                                           direction.z * direction.z);
            if (length > 0.001f)
                return CVector(direction.x / length, direction.y / length, direction.z / length);
        }

        // Use the camera matrix's forward axis if the active CCam has not settled.
        const CVector direction = TheCamera.m_mCameraMatrix.up;
        const float length = std::sqrt(direction.x * direction.x +
                                       direction.y * direction.y +
                                       direction.z * direction.z);
        if (length > 0.001f)
            return CVector(direction.x / length, direction.y / length, direction.z / length);

        return CVector(0.0f, 1.0f, 0.0f);
    }

    void CaptureCameraPose()
    {
        g_cameraPosition = TheCamera.m_mCameraMatrix.pos;
        g_cameraForward = GetCameraForwardFromActiveCam();
        g_cameraYaw = std::atan2(g_cameraForward.y, g_cameraForward.x);
        g_cameraPitch = std::asin(ClampFloat(g_cameraForward.z, -1.0f, 1.0f));

        // CDraw::ms_fFOV is the projection FOV used by the renderer. Reading
        // only CCam::m_fFOV can appear to work in memory while leaving the
        // actual rendered view unchanged, so use the render FOV as the source.
        g_cameraFov = CDraw::ms_fFOV;
        if (!std::isfinite(g_cameraFov) || g_cameraFov < g_settings.minFov || g_cameraFov > g_settings.maxFov)
        {
            g_cameraFov = (TheCamera.m_nActiveCam < 3)
                ? TheCamera.m_aCams[TheCamera.m_nActiveCam].m_fFOV
                : DEFAULT_FOV;
        }

        if (!std::isfinite(g_cameraFov) || g_cameraFov < g_settings.minFov || g_cameraFov > g_settings.maxFov)
            g_cameraFov = DEFAULT_FOV;

        g_cameraFov = ClampFloat(g_cameraFov, g_settings.minFov, g_settings.maxFov);
    }

    CVector CalculateForwardVector()
    {
        const float horizontal = std::cos(g_cameraPitch);
        return CVector(
            horizontal * std::cos(g_cameraYaw),
            horizontal * std::sin(g_cameraYaw),
            std::sin(g_cameraPitch));
    }

    void ActivateFixedCamera()
    {
        g_cameraForward = CalculateForwardVector();
        const CVector lookAt(
            g_cameraPosition.x + g_cameraForward.x,
            g_cameraPosition.y + g_cameraForward.y,
            g_cameraPosition.z + g_cameraForward.z);
        const CVector upOffset(0.0f, 0.0f, 1.0f);

        TheCamera.TakeControlNoEntity(&lookAt, SWITCHTYPE_JUMPCUT, 1);
        TheCamera.m_vecFixedModeSource = g_cameraPosition;
        TheCamera.m_vecFixedModeVector = lookAt;
        TheCamera.m_vecFixedModeUpOffSet = upOffset;
        TheCamera.SetCamPositionForFixedMode(&g_cameraPosition, &upOffset);
    }

    void ApplyFreeCamera()
    {
        if (!g_bScreenshotMode || !g_bFreeCamera || TheCamera.m_nActiveCam >= 3)
            return;

        g_cameraForward = CalculateForwardVector();
        const CVector lookAt(
            g_cameraPosition.x + g_cameraForward.x,
            g_cameraPosition.y + g_cameraForward.y,
            g_cameraPosition.z + g_cameraForward.z);
        const CVector upOffset(0.0f, 0.0f, 1.0f);

        // Update only the fixed-camera target/source consumed by GTA's own
        // CCam::Process_Fixed. Let the game's camera pipeline build the basis,
        // view matrix, inverse matrix and frustum together. Writing individual
        // m_mCameraMatrix axes here made those structures disagree and could
        // invert/distort world rendering when the mode was activated.
        TheCamera.m_vecFixedModeSource = g_cameraPosition;
        TheCamera.m_vecFixedModeVector = lookAt;
        TheCamera.m_vecFixedModeUpOffSet = upOffset;
        TheCamera.SetCamPositionForFixedMode(&g_cameraPosition, &upOffset);

        CCam& camera = TheCamera.m_aCams[TheCamera.m_nActiveCam];
        camera.m_vecCamFixedModeSource = g_cameraPosition;
        camera.m_vecCamFixedModeVector = lookAt;
        camera.m_vecCamFixedModeUpOffSet = upOffset;
        camera.m_fFOV = g_cameraFov;

        // The screenshot mode no longer patches bytes inside CCamera::Process.
        // Set the projection FOV last from the render callback instead of
        // jumping over native camera instructions at a fixed executable address.
        CDraw::SetFOV(g_cameraFov);
        CDraw::ms_fFOV = g_cameraFov;
    }

    bool WriteCodeBytes(std::uintptr_t address, const BYTE* values, std::size_t count)
    {
        if (!values || count == 0)
            return false;

        void* const target = reinterpret_cast<void*>(address);
        DWORD oldProtection = 0;
        if (!VirtualProtect(target, count, PAGE_EXECUTE_READWRITE, &oldProtection))
            return false;

        volatile BYTE* destination = reinterpret_cast<volatile BYTE*>(target);
        for (std::size_t i = 0; i < count; ++i)
            destination[i] = values[i];

        FlushInstructionCache(GetCurrentProcess(), target, count);

        DWORD ignoredProtection = 0;
        VirtualProtect(target, count, oldProtection, &ignoredProtection);
        return true;
    }

    bool WriteCodeByte(std::uintptr_t address, BYTE value)
    {
        return WriteCodeBytes(address, &value, sizeof(value));
    }

    bool ApplySceneEffectsFreezePatches()
    {
        if (g_bSceneEffectsPatchesApplied)
            return true;

        // Preserve the bytes present at runtime, including a possible compatible
        // pre-existing hook, and restore them when effect freezing is disabled.
        for (std::size_t i = 0; i < sizeof(g_savedFxManagerUpdateBytes); ++i)
            g_savedFxManagerUpdateBytes[i] = *reinterpret_cast<volatile BYTE*>(FX_MANAGER_UPDATE_ADDRESS + i);
        g_savedCoronasUpdateByte = *reinterpret_cast<volatile BYTE*>(CORONAS_UPDATE_ADDRESS);

        if (!WriteCodeBytes(FX_MANAGER_UPDATE_ADDRESS,
                            FX_MANAGER_RETURN_WITH_STACK_CLEANUP,
                            sizeof(FX_MANAGER_RETURN_WITH_STACK_CLEANUP)))
            return false;

        if (!WriteCodeByte(CORONAS_UPDATE_ADDRESS, NATIVE_RETURN_OPCODE))
        {
            WriteCodeBytes(FX_MANAGER_UPDATE_ADDRESS,
                           g_savedFxManagerUpdateBytes,
                           sizeof(g_savedFxManagerUpdateBytes));
            return false;
        }

        g_bSceneEffectsPatchesApplied = true;
        return true;
    }

    void RestoreSceneEffectsPatches()
    {
        if (!g_bSceneEffectsPatchesApplied)
        {
            g_bSceneEffectsFrozen = false;
            return;
        }

        WriteCodeBytes(FX_MANAGER_UPDATE_ADDRESS,
                      g_savedFxManagerUpdateBytes,
                      sizeof(g_savedFxManagerUpdateBytes));
        WriteCodeByte(CORONAS_UPDATE_ADDRESS, g_savedCoronasUpdateByte);
        g_bSceneEffectsPatchesApplied = false;
        g_bSceneEffectsFrozen = false;
    }

    void SetSceneEffectsFrozen(bool freeze)
    {
        if (freeze)
            g_bSceneEffectsFrozen = ApplySceneEffectsFreezePatches();
        else
            RestoreSceneEffectsPatches();
    }

    void SuppressNativeHudDrawing()
    {
        if (g_bNativeHudDrawingSuppressed)
            return;

        // Save the bytes as they are at runtime so a compatible hook installed
        // by another plugin, if present, is restored rather than overwritten
        // with a hard-coded original opcode.
        g_savedHudDrawOpcode = *reinterpret_cast<volatile BYTE*>(HUD_DRAW_ADDRESS);
        g_savedHudDrawAfterFadeOpcode = *reinterpret_cast<volatile BYTE*>(HUD_DRAW_AFTER_FADE_ADDRESS);

        if (!WriteCodeByte(HUD_DRAW_ADDRESS, NATIVE_RETURN_OPCODE))
            return;

        if (!WriteCodeByte(HUD_DRAW_AFTER_FADE_ADDRESS, NATIVE_RETURN_OPCODE))
        {
            WriteCodeByte(HUD_DRAW_ADDRESS, g_savedHudDrawOpcode);
            return;
        }

        g_bNativeHudDrawingSuppressed = true;
    }

    void RestoreNativeHudDrawing()
    {
        if (!g_bNativeHudDrawingSuppressed)
            return;

        WriteCodeByte(HUD_DRAW_ADDRESS, g_savedHudDrawOpcode);
        WriteCodeByte(HUD_DRAW_AFTER_FADE_ADDRESS, g_savedHudDrawAfterFadeOpcode);
        g_bNativeHudDrawingSuppressed = false;
    }

    void HideInterface()
    {
        // m_Wants_To_Draw_Hud and the script flags hide the standard elements;
        // temporarily bypassing both native HUD render passes also suppresses
        // overlays/messages drawn outside those individual flags.
        SuppressNativeHudDrawing();
        CHud::m_Wants_To_Draw_Hud = false;
        CHud::bScriptDontDisplayRadar = true;
        CHud::bScriptDontDisplayAreaName = true;
        CHud::bScriptDontDisplayVehicleName = true;
        CHud::bDrawClock = false;
        CHud::m_bDraw3dMarkers = false;
    }

    void RestoreInterface()
    {
        CHud::m_Wants_To_Draw_Hud = g_savedHudVisible;
        CHud::bScriptDontDisplayRadar = g_savedRadarHidden;
        CHud::bScriptDontDisplayAreaName = g_savedAreaNameHidden;
        CHud::bScriptDontDisplayVehicleName = g_savedVehicleNameHidden;
        CHud::bDrawClock = g_savedClockVisible;
        CHud::m_bDraw3dMarkers = g_saved3dMarkers;
    }

    void SaveScreenshotModeState()
    {
        g_savedTimeScale = CTimer::ms_fTimeScale;
        g_savedHudVisible = CHud::m_Wants_To_Draw_Hud;
        g_savedRadarHidden = CHud::bScriptDontDisplayRadar;
        g_savedAreaNameHidden = CHud::bScriptDontDisplayAreaName;
        g_savedVehicleNameHidden = CHud::bScriptDontDisplayVehicleName;
        g_savedClockVisible = CHud::bDrawClock;
        g_saved3dMarkers = CHud::m_bDraw3dMarkers;
        g_savedWideScreen = TheCamera.m_bWideScreenOn;
        g_bWideScreenCurrent = g_savedWideScreen;
        g_savedDrawFov = CDraw::ms_fFOV;
        CPad* pad = CPad::GetPad(0);
        g_savedPlayerControlState = pad ? pad->DisablePlayerControls : 0;

        for (int i = 0; i < 3; ++i)
            g_savedCameraFov[i] = TheCamera.m_aCams[i].m_fFOV;

        g_modeInitialFov = g_savedDrawFov;
        if (!std::isfinite(g_modeInitialFov) || g_modeInitialFov < g_settings.minFov || g_modeInitialFov > g_settings.maxFov)
        {
            g_modeInitialFov = (TheCamera.m_nActiveCam < 3)
                ? TheCamera.m_aCams[TheCamera.m_nActiveCam].m_fFOV
                : DEFAULT_FOV;
        }
        if (!std::isfinite(g_modeInitialFov) || g_modeInitialFov < g_settings.minFov || g_modeInitialFov > g_settings.maxFov)
            g_modeInitialFov = DEFAULT_FOV;
    }

    void ChangeGameHour(int direction)
    {
        int hour = static_cast<int>(CClock::ms_nGameClockHours);
        hour = (hour + direction * g_settings.hoursPerPress) % 24;
        if (hour < 0)
            hour += 24;

        CClock::SetGameClock(static_cast<unsigned char>(hour),
                            CClock::ms_nGameClockMinutes,
                            CClock::ms_nGameClockDays);
    }

    void SelectWeatherPreset(short weatherId)
    {
        if (weatherId < 0 || weatherId > 22)
            return;

        // ForceWeather keeps the selected preset forced; ForceWeatherNow applies
        // the visual change immediately rather than waiting for the normal cycle.
        CWeather::ForceWeather(weatherId);
        CWeather::ForceWeatherNow(weatherId);
        CWeather::ForcedWeatherType = weatherId;
        g_selectedWeatherPreset = weatherId;
    }

    bool WasDigitPressed(int digit)
    {
        if (digit < 0 || digit > 9)
            return false;
        const int topRowKey = '0' + digit;
        const int numpadKey = VK_NUMPAD0 + digit;
        const bool topRowPressed = WasKeyPressed(topRowKey);
        const bool numpadPressed = WasKeyPressed(numpadKey);
        return topRowPressed || numpadPressed;
    }

    void ProcessWeatherHotkeys()
    {
        // Number row and numpad are both supported. Consume edge states for every
        // digit each frame, so one held key cannot leave another digit's latch stale.
        const bool shift = IsKeyDown(VK_SHIFT);
        const bool control = IsKeyDown(VK_CONTROL);
        int pressedDigit = -1;
        for (int digit = 1; digit <= 9; ++digit)
        {
            const bool pressed = WasDigitPressed(digit);
            if (pressed && pressedDigit < 0)
                pressedDigit = digit;
        }
        const bool zeroPressed = WasDigitPressed(0);

        if (pressedDigit >= 1)
        {
            if (control)
            {
                if (pressedDigit <= 4)
                    SelectWeatherPreset(static_cast<short>(18 + pressedDigit)); // 19-22
            }
            else if (shift)
                SelectWeatherPreset(static_cast<short>(9 + pressedDigit)); // 10-18
            else
                SelectWeatherPreset(static_cast<short>(pressedDigit - 1)); // 0-8
        }
        else if (zeroPressed && !shift && !control)
            SelectWeatherPreset(9);
    }

    void ProcessEnvironmentHotkeys()
    {
        // Draw-distance controls are temporarily disabled. The previous version
        // wrote to CDraw/CRenderer LOD and far-clip globals every frame; those
        // writes are intentionally removed while diagnosing the broken render.
        ProcessWeatherHotkeys();

        const bool usingDefaultHourKeys =
            g_settings.keyHourDown == VK_OEM_MINUS &&
            g_settings.keyHourUp == VK_OEM_PLUS;
        bool hourDownPressed = false;
        bool hourUpPressed = false;
        if (usingDefaultHourKeys)
        {
            const bool minusCharacterPressed = WasMappedCharacterPressed('-');
            const bool plusCharacterPressed = WasMappedCharacterPressed('+');
            const bool mainMinusPressed = WasKeyPressed(VK_OEM_MINUS) && !IsKeyDown(VK_SHIFT);
            const bool mainPlusKeyPressed = WasKeyPressed(VK_OEM_PLUS) && IsKeyDown(VK_SHIFT);
            const bool numMinusPressed = WasKeyPressed(VK_SUBTRACT);
            const bool numPlusPressed = WasKeyPressed(VK_ADD);
            hourDownPressed = minusCharacterPressed || mainMinusPressed || numMinusPressed;
            hourUpPressed = plusCharacterPressed || mainPlusKeyPressed || numPlusPressed;
        }
        else
        {
            hourDownPressed = WasKeyPressed(g_settings.keyHourDown);
            hourUpPressed = WasKeyPressed(g_settings.keyHourUp);
        }

        if (hourDownPressed) ChangeGameHour(-1);
        if (hourUpPressed) ChangeGameHour(1);
    }

    void DrawControlHelp()
    {
        if (!g_bScreenshotMode || !g_bControlHelpVisible)
            return;

        // Keep the help compact and unobtrusive. FontSize in AmazingSnapshot.ini
        // scales both axes proportionally to the default subtitle font size.
        const float fontRatio = g_settings.helpFontSize / 0.38f;
        const float fontScaleX = g_settings.helpFontSize;
        const float fontScaleY = 0.80f * fontRatio;
        const float lineSpacing = 18.0f * fontRatio;
        CFont::SetFontStyle(FONT_SUBTITLES);
        CFont::SetColor(CRGBA(255, 255, 255, 255));
        CFont::SetDropShadowPosition(1);
        CFont::SetBackground(false, false);
        CFont::SetProportional(true);
        CFont::SetOrientation(ALIGN_LEFT);
        CFont::SetWrapx(620.0f);

        const std::string lines[] = {
            "Alt+" + GetVirtualKeyLabel(g_settings.keyModeToggle) + ": toggle screenshot mode",
            GetVirtualKeyLabel(g_settings.keyForward) + "/" + GetVirtualKeyLabel(g_settings.keyLeft) + "/" +
                GetVirtualKeyLabel(g_settings.keyBackward) + "/" + GetVirtualKeyLabel(g_settings.keyRight) + " / arrows: move",
            GetVirtualKeyLabel(g_settings.keyAscend) + " / PgUp: rise    " + GetVirtualKeyLabel(g_settings.keyDescend) + " / PgDn: lower",
            "Mouse: look    LMB: lock X    RMB: lock Y",
            GetVirtualKeyLabel(g_settings.keyMoveFast) + ": faster    " + GetVirtualKeyLabel(g_settings.keyMoveSlow) + ": slower",
            GetVirtualKeyLabel(g_settings.keyZoomIn) + "/" + GetVirtualKeyLabel(g_settings.keyZoomOut) + ": zoom    " +
                GetVirtualKeyLabel(g_settings.keyResetZoom) + ": reset FOV",
            GetVirtualKeyLabel(g_settings.keyCapturePhoto) + ": take photo",
            GetVirtualKeyLabel(g_settings.keyFreezeEffects) + ": effects hold " + (g_bSceneEffectsFrozen ? "ON" : "OFF"),
            GetVirtualKeyLabel(g_settings.keyWideScreen) + ": widescreen    Esc: exit",
            GetVirtualKeyLabel(g_settings.keyHourDown) + "/" + GetVirtualKeyLabel(g_settings.keyHourUp) + ": hour",
            "1-0: weather 0-9    Shift+1-9: weather 10-18",
            "Ctrl+1-4: weather 19-22",
            GetVirtualKeyLabel(g_settings.keyShowHelp) + ": hide/show this menu",
            "FOV " + std::to_string(static_cast<int>(std::round(g_cameraFov))) +
                " | zoom key " + (g_bZoomKeyActive ? "ON" : "off") +
                " | time " + (CClock::ms_nGameClockHours < 10 ? "0" : "") +
                std::to_string(static_cast<int>(CClock::ms_nGameClockHours)) + ":" +
                (CClock::ms_nGameClockMinutes < 10 ? "0" : "") +
                std::to_string(static_cast<int>(CClock::ms_nGameClockMinutes)) +
                " | weather " + std::to_string(g_selectedWeatherPreset >= 0 ? g_selectedWeatherPreset : CWeather::ForcedWeatherType)
        };

        const unsigned int lineCount = static_cast<unsigned int>(sizeof(lines) / sizeof(lines[0]));
        for (unsigned int i = 0; i < lineCount; ++i)
        {
            // Reapply the scale before every line. The game shares CFont state
            // globally with HUD/script text, so another renderer can overwrite
            // it between calls in some plugin combinations.
            CFont::SetScale(fontScaleX, fontScaleY);
            CFont::PrintString(22.0f, 24.0f + static_cast<float>(i) * lineSpacing, lines[i].c_str());
        }
    }

    // Invoke the game's own TAKE_PHOTO SCM command (opcode 0x0A1E).
    // It captures the current screen without HUD and lets GTA SA save the photo
    // to its native Gallery directory. The argument is the game's boolean flag.
    void TakePhotoUsingGameSystem()
    {
        static unsigned char photoCommand[] = {
            0x1E, 0x0A, // opcode 0x0A1E: TAKE_PHOTO
            SCRIPTPARAM_STATIC_INT_8BITS, 1,
            0x00, 0x00 // harmless NOP terminator after the one command
        };

        CRunningScript script = {};
        script.m_pBaseIP = photoCommand;
        script.m_pCurrentIP = photoCommand;
        script.m_bIsActive = true;
        script.m_bCondResult = true;
        script.m_bIsMission = false;
        script.ProcessOneCommand();
    }

    void EnterScreenshotMode()
    {
        if (g_bScreenshotMode)
            return;

        SaveScreenshotModeState();
        // Do not patch CCamera::Process: camera pose and projection are applied
        // through Plugin-SDK camera APIs instead of skipping native instructions.
        CaptureCameraPose();
        g_modeInitialFov = DEFAULT_FOV; // original script's P reset value
        g_bZoomKeyActive = false;

        g_bScreenshotMode = true;
        g_bFreeCamera = true;
        g_lastUpdateTick = GetTickCount();

        // Time is frozen by the game's time-scale variable. Camera movement uses
        // GetTickCount so it remains responsive while the simulation is frozen.
        if (g_settings.freezeTime)
            CTimer::ms_fTimeScale = 0.0f;
        if (g_settings.hideHud)
            HideInterface();
        // Hold already-created FX particles and corona fading so the scene can
        // be viewed from different camera angles without short effects expiring.
        SetSceneEffectsFrozen(g_settings.freezeEffectsOnStart);

        if (CPad* pad = CPad::GetPad(0))
            pad->DisablePlayerControls = 1;

        // The controls panel is configurable and can be toggled with the help key.
        g_bControlHelpVisible = g_settings.showHelpOnStart;
        ActivateFixedCamera();
    }

    void ExitScreenshotMode()
    {
        if (!g_bScreenshotMode)
            return;

        g_bScreenshotMode = false;
        g_bFreeCamera = false;
        g_bControlHelpVisible = false;
        g_bPhotoCaptureRequested = false;
        RestoreSceneEffectsPatches();
        g_bZoomKeyActive = false;

        // Restore the standard game camera before returning the saved globals.
        TheCamera.RestoreWithJumpCut();
        for (int i = 0; i < 3; ++i)
            TheCamera.m_aCams[i].m_fFOV = g_savedCameraFov[i];
        CDraw::SetFOV(g_savedDrawFov);
        CDraw::ms_fFOV = g_savedDrawFov;

        if (g_savedWideScreen)
            TheCamera.SetWideScreenOn();
        else
            TheCamera.SetWideScreenOff();

        RestoreNativeHudDrawing();
        RestoreInterface();
        CTimer::ms_fTimeScale = g_savedTimeScale;

        if (CPad* pad = CPad::GetPad(0))
            pad->DisablePlayerControls = g_savedPlayerControlState;

    }

    void ToggleWideScreen()
    {
        g_bWideScreenCurrent = !g_bWideScreenCurrent;
        if (g_bWideScreenCurrent)
            TheCamera.SetWideScreenOn();
        else
            TheCamera.SetWideScreenOff();
    }

    void UpdateFreeCameraInput()
    {
        if (!g_bScreenshotMode || !g_bFreeCamera)
            return;

        ProcessEnvironmentHotkeys();

        const unsigned int now = GetTickCount();
        float deltaTime = static_cast<float>(now - g_lastUpdateTick) / 1000.0f;
        g_lastUpdateTick = now;
        deltaTime = ClampFloat(deltaTime, 0.001f, MAX_FRAME_DELTA);

        const bool lockYaw = IsKeyDown(VK_LBUTTON);   // Original script: LMB locks mouse X.
        const bool lockPitch = IsKeyDown(VK_RBUTTON); // Original script: RMB locks mouse Y.

        const CMouseControllerState& mouse = CPad::NewMouseControllerState;
        // Horizontal mouse direction is reversed to correct left/right inversion.
        // Keep vertical input unchanged because the vertical axis already behaves correctly.
        if (!lockYaw)
            g_cameraYaw -= mouse.x * g_settings.mouseSensitivity;
        if (!lockPitch)
            g_cameraPitch += mouse.y * g_settings.mouseSensitivity;
        g_cameraPitch = ClampFloat(g_cameraPitch, MIN_PITCH, MAX_PITCH);

        // Original Amazing Screenshot calls CDraw::SetFOV while [ or ] is held,
        // changing FOV by 0.5 degrees each update, within 10..150 degrees. The
        // engine-side FOV reset is patched for screenshot mode so it cannot undo
        // the requested zoom on the next camera update.
        const bool defaultZoomKeys =
            g_settings.keyZoomIn == VK_OEM_4 && g_settings.keyZoomOut == VK_OEM_6;
        const bool zoomIn = IsKeyDown(g_settings.keyZoomIn) ||
            (defaultZoomKeys && IsMappedCharacterDown('[')) || IsKeyDown(VK_ADD);
        const bool zoomOut = IsKeyDown(g_settings.keyZoomOut) ||
            (defaultZoomKeys && IsMappedCharacterDown(']')) || IsKeyDown(VK_SUBTRACT);
        g_bZoomKeyActive = zoomIn || zoomOut;
        if (zoomIn && !zoomOut)
            g_cameraFov -= g_settings.zoomStep;
        if (zoomOut && !zoomIn)
            g_cameraFov += g_settings.zoomStep;
        if (WasKeyPressed(g_settings.keyResetZoom))
            g_cameraFov = g_modeInitialFov;
        g_cameraFov = ClampFloat(g_cameraFov, g_settings.minFov, g_settings.maxFov);

        if (WasKeyPressed(g_settings.keyWideScreen))
            ToggleWideScreen();

        const float horizontalX = std::cos(g_cameraYaw);
        const float horizontalY = std::sin(g_cameraYaw);
        const float rightX = std::sin(g_cameraYaw);
        const float rightY = -std::cos(g_cameraYaw);

        float forwardInput = 0.0f;
        float rightInput = 0.0f;
        float verticalInput = 0.0f;
        if (IsKeyDown(g_settings.keyForward) || IsKeyDown(VK_UP)) forwardInput += 1.0f;
        if (IsKeyDown(g_settings.keyBackward) || IsKeyDown(VK_DOWN)) forwardInput -= 1.0f;
        if (IsKeyDown(g_settings.keyRight) || IsKeyDown(VK_RIGHT)) rightInput += 1.0f;
        if (IsKeyDown(g_settings.keyLeft) || IsKeyDown(VK_LEFT)) rightInput -= 1.0f;
        if (IsKeyDown(g_settings.keyAscend) || IsKeyDown(VK_PRIOR)) verticalInput += 1.0f; // Space / Page Up
        if (IsKeyDown(g_settings.keyDescend) || IsKeyDown(VK_NEXT)) verticalInput -= 1.0f;  // C / Page Down

        float speed = g_settings.baseMoveSpeed;
        if (IsKeyDown(g_settings.keyMoveFast)) speed *= g_settings.fastMoveMultiplier;
        if (IsKeyDown(g_settings.keyMoveSlow)) speed *= g_settings.slowMoveMultiplier;

        g_cameraPosition.x += (horizontalX * forwardInput + rightX * rightInput) * speed * deltaTime;
        g_cameraPosition.y += (horizontalY * forwardInput + rightY * rightInput) * speed * deltaTime;
        g_cameraPosition.z += verticalInput * speed * deltaTime;
        g_cameraForward = CalculateForwardVector();
    }

    void ProcessScreenshotHotkeys()
    {
        const bool vPressed = WasKeyPressed(g_settings.keyModeToggle);
        const bool altDown = IsKeyDown(VK_MENU) || IsKeyDown(VK_LMENU) || IsKeyDown(VK_RMENU);

        if (vPressed && altDown)
        {
            if (g_bScreenshotMode)
                ExitScreenshotMode();
            else
                EnterScreenshotMode();
            return;
        }

        // V without Alt is intentionally ignored while screenshot mode is active.
        // The camera stays in free-camera mode until Alt+V or Esc exits the mode.

        if (g_bScreenshotMode && IsKeyDown(VK_ESCAPE))
            ExitScreenshotMode();
    }

    void ProcessScreenshotMode()
    {
        ProcessScreenshotHotkeys();
        if (!g_bScreenshotMode)
            return;

        if (WasKeyPressed(g_settings.keyShowHelp))
            g_bControlHelpVisible = !g_bControlHelpVisible;

        if (WasKeyPressed(g_settings.keyFreezeEffects))
            SetSceneEffectsFrozen(!g_bSceneEffectsFrozen);

        // Defer capture until the drawing callback has applied the latest camera
        // matrix. The actual image is saved by GTA's native TAKE_PHOTO opcode.
        if (WasKeyPressed(g_settings.keyCapturePhoto))
            g_bPhotoCaptureRequested = true;

        // Re-assert state while capture mode is active in case another game system
        // briefly overwrites it. Original values are restored on exit.
        if (g_settings.freezeTime)
            CTimer::ms_fTimeScale = 0.0f;
        if (g_settings.hideHud)
            HideInterface();
        if (CPad* pad = CPad::GetPad(0))
            pad->DisablePlayerControls = 1;

        UpdateFreeCameraInput();
        ApplyFreeCamera();
    }

    void RenderScreenshotCamera()
    {
        if (g_bScreenshotMode)
            if (g_bScreenshotMode && g_bFreeCamera)
            ApplyFreeCamera();

        // Process the game's native photo command after synchronizing our camera,
        // before drawing the optional control overlay so the help text is not
        // baked into the picture.
        if (g_bPhotoCaptureRequested)
        {
            g_bPhotoCaptureRequested = false;
            TakePhotoUsingGameSystem();
        }

        DrawControlHelp();
    }

    struct Bootstrap
    {
        Bootstrap()
        {
            LoadConfiguration();
            plugin::Events::gameProcessEvent += ProcessScreenshotMode;
            plugin::Events::drawingEvent += RenderScreenshotCamera;
        }
    } g_bootstrap;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
