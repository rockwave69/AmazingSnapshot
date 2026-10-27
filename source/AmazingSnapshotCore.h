#pragma once

// Shared core for GTA III, Vice City and GTA San Andreas. It provides common config/input/camera
// utilities and the unified RenderWare-to-JPEG Gallery writer used by GTA III and Vice City.
// Keyboard bindings are physical Set-1 scan codes, not translated characters/VK symbols.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <array>
#include <string>

namespace AmazingSnapshot
{
    // Physical keyboard scan-code identifiers. Bit 0x100 marks an E0/extended key.
    // Values below 0x200 are hardware scan-code IDs; the three values at 0x200+
    // are logical aliases which mean either side of the modifier key.
    namespace KeyCode
    {
        constexpr int ScanCodeCount = 0x200;
        constexpr int Shift = 0x200;
        constexpr int Control = 0x201;
        constexpr int Alt = 0x202;
        constexpr std::size_t StateCount = 0x203;

        constexpr int Escape = 0x001;
        constexpr int Digit1 = 0x002;
        constexpr int Digit2 = 0x003;
        constexpr int Digit3 = 0x004;
        constexpr int Digit4 = 0x005;
        constexpr int Digit5 = 0x006;
        constexpr int Digit6 = 0x007;
        constexpr int Digit7 = 0x008;
        constexpr int Digit8 = 0x009;
        constexpr int Digit9 = 0x00A;
        constexpr int Digit0 = 0x00B;
        constexpr int Minus = 0x00C;
        constexpr int Equals = 0x00D; // Same physical key whether it types '=' or '+'
        constexpr int Backspace = 0x00E;
        constexpr int Tab = 0x00F;
        constexpr int Q = 0x010;
        constexpr int W = 0x011;
        constexpr int E = 0x012;
        constexpr int R = 0x013;
        constexpr int T = 0x014;
        constexpr int Y = 0x015;
        constexpr int U = 0x016;
        constexpr int I = 0x017;
        constexpr int O = 0x018;
        constexpr int P = 0x019;
        constexpr int LeftBracket = 0x01A;
        constexpr int RightBracket = 0x01B;
        constexpr int Enter = 0x01C;
        constexpr int LeftControl = 0x01D;
        constexpr int A = 0x01E;
        constexpr int S = 0x01F;
        constexpr int D = 0x020;
        constexpr int F = 0x021;
        constexpr int G = 0x022;
        constexpr int H = 0x023;
        constexpr int J = 0x024;
        constexpr int K = 0x025;
        constexpr int L = 0x026;
        constexpr int LeftShift = 0x02A;
        constexpr int Z = 0x02C;
        constexpr int X = 0x02D;
        constexpr int C = 0x02E;
        constexpr int V = 0x02F;
        constexpr int B = 0x030;
        constexpr int N = 0x031;
        constexpr int M = 0x032;
        constexpr int Comma = 0x033;
        constexpr int Period = 0x034;
        constexpr int RightShift = 0x036;
        constexpr int LeftAlt = 0x038;
        constexpr int Space = 0x039;
        constexpr int F1 = 0x03B;
        constexpr int F2 = 0x03C;
        constexpr int F3 = 0x03D;
        constexpr int F4 = 0x03E;
        constexpr int F5 = 0x03F;
        constexpr int F6 = 0x040;
        constexpr int F7 = 0x041;
        constexpr int F8 = 0x042;
        constexpr int F9 = 0x043;
        constexpr int F10 = 0x044;
        constexpr int Num7 = 0x047;
        constexpr int Num8 = 0x048;
        constexpr int Num9 = 0x049;
        constexpr int NumMinus = 0x04A;
        constexpr int Num4 = 0x04B;
        constexpr int Num5 = 0x04C;
        constexpr int Num6 = 0x04D;
        constexpr int NumPlus = 0x04E;
        constexpr int Num1 = 0x04F;
        constexpr int Num2 = 0x050;
        constexpr int Num3 = 0x051;
        constexpr int Num0 = 0x052;
        constexpr int F11 = 0x057;
        constexpr int F12 = 0x058;
        constexpr int IsoLessGreater = 0x056;

        constexpr int NumEnter = 0x11C;
        constexpr int RightControl = 0x11D;
        constexpr int NumDivide = 0x135;
        constexpr int RightAlt = 0x138;
        constexpr int Home = 0x147;
        constexpr int Up = 0x148;
        constexpr int PageUp = 0x149;
        constexpr int Left = 0x14B;
        constexpr int Right = 0x14D;
        constexpr int End = 0x14F;
        constexpr int Down = 0x150;
        constexpr int PageDown = 0x151;
        constexpr int Insert = 0x152;
        constexpr int Delete = 0x153;

        inline int NumberRowKey(int digit)
        {
            return digit == 0 ? Digit0 : (digit >= 1 && digit <= 9 ? Digit1 + digit - 1 : -1);
        }

        inline int NumpadKey(int digit)
        {
            switch (digit)
            {
            case 0: return Num0;
            case 1: return Num1;
            case 2: return Num2;
            case 3: return Num3;
            case 4: return Num4;
            case 5: return Num5;
            case 6: return Num6;
            case 7: return Num7;
            case 8: return Num8;
            case 9: return Num9;
            default: return -1;
            }
        }
    }

    struct Settings
    {
        bool m_freezeTime;
        bool m_hideHud;
        bool m_freezeEffectsOnStart;
        bool m_showHelpOnStart;
        float m_minFov;
        float m_maxFov;
        float m_zoomStep;
        float m_mouseSensitivity;
        float m_baseMoveSpeed;
        float m_fastMoveMultiplier;
        float m_slowMoveMultiplier;
        int m_keyModeToggle;
        int m_keyToggleTimePlayback;
        int m_keyWideScreen;
        int m_keyZoomIn;
        int m_keyZoomOut;
        int m_keyResetZoom;
        int m_keyShowHelp;
        int m_keyFreezeEffects;
        int m_keyCapturePhoto;
        int m_keyAscend;
        int m_keyDescend;
        int m_keyForward;
        int m_keyBackward;
        int m_keyLeft;
        int m_keyRight;
        int m_keyMoveFast;
        int m_keyMoveSlow;
        float m_helpFontSize;
        int m_hoursPerPress;
        int m_keyHourDown;
        int m_keyHourUp;
    };

    struct Vector3
    {
        float x;
        float y;
        float z;
    };

    class InputTracker
    {
    public:
        // Call once per game tick before checking keys. Installs the physical-key hook lazily,
        // outside DllMain/static constructors.
        bool Initialize();
        void Shutdown();
        bool IsKeyDown(int scanCode) const;
        bool WasKeyPressed(int scanCode);
        bool IsMouseButtonDown(int virtualKey) const;
        // When active, suppress keyboard events from reaching the game window so
        // game-level hotkeys (for example replay controls) cannot run in parallel.
        // The tracker still observes physical key state for the screenshot controls.
        void SetCaptureMode(bool active);
        // Suppress only Alt+mode-toggle-key from reaching the game even while
        // capture mode is inactive; a plain press of that key still reaches GTA.
        void SetModeToggleKey(int scanCode);
        // True while Esc-opened game menu navigation is being forwarded.
        bool IsGameMenuInputActive() const;
        void SetGameMenuInputActive(bool active);
        void Reset();

    private:
        std::array<bool, KeyCode::StateCount> m_previousKeys{};
    };

    Settings MakeDefaultSettings();
    void LoadSettings(Settings& settings, const char* iniFileName = "AmazingSnapshot.ini");
    const char* GetConfigurationPath(const char* iniFileName = "AmazingSnapshot.ini");
    std::string GetKeyLabel(int scanCode);
    // Common physical-key help lines shared by all three game adapters. Labels are
    // fixed to US physical key positions and must not depend on the active layout.
    std::array<std::string, 6> BuildCommonCameraHelpLines(const Settings& settings);
    std::string BuildCameraModeHelpLine(bool freeCameraEnabled);
    std::string BuildCameraTimeHelpLine(bool freeCameraEnabled, bool timePlaybackEnabled, bool gameMenuActive);
    std::string BuildHudHelpLine(bool hudHidden);
    std::string BuildHudTimeHelpLine(bool hudHidden, bool timeFrozen);
    // A short, identical activation cue for all three supported games.
    void PlayModeActivationSound();
    float ClampFloat(float value, float minimum, float maximum);
    // Same per-render-frame FOV zoom behavior for the free camera in III/VC/SA.
    float UpdateContinuousCameraFov(float currentFov, float initialFov, const Settings& settings,
        bool zoomInDown, bool zoomOutDown, bool resetPressed);

    // Resolve/create the Gallery directory. Prefer <game exe>\userfiles\Gallery,
    // falling back to Documents\<gameUserFilesDirectory>\Gallery if local writes fail.
    bool GetGalleryDirectory(const char* gameUserFilesDirectory, std::string& outputPath,
        bool& usingPortablePath);

    // Capture a RenderWare camera raster and save it as galleryN.jpg in the selected Gallery.
    // The camera argument is intentionally opaque so adapters share this without exposing game-specific SDK types.
    bool SaveRenderWareFrameToGalleryJpeg(void* renderWareCamera, const char* gameUserFilesDirectory,
        std::string& savedPath);

    Vector3 ForwardFromYawPitch(float yaw, float pitch);
    Vector3 RightFromYaw(float yaw);
    Vector3 Normalize(const Vector3& vector, const Vector3& fallback);
}
