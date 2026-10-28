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
#include <cstddef>
#include <cstdint>
#include <cmath>
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
        constexpr int Semicolon = 0x027;
        constexpr int Apostrophe = 0x028;
        constexpr int Grave = 0x029;
        constexpr int LeftShift = 0x02A;
        constexpr int Backslash = 0x02B;
        constexpr int Z = 0x02C;
        constexpr int X = 0x02D;
        constexpr int C = 0x02E;
        constexpr int V = 0x02F;
        constexpr int B = 0x030;
        constexpr int N = 0x031;
        constexpr int M = 0x032;
        constexpr int Comma = 0x033;
        constexpr int Period = 0x034;
        constexpr int Slash = 0x035;
        constexpr int RightShift = 0x036;
        constexpr int NumMultiply = 0x037;
        constexpr int LeftAlt = 0x038;
        constexpr int Space = 0x039;
        constexpr int CapsLock = 0x03A;
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
        constexpr int NumLock = 0x045;
        constexpr int ScrollLock = 0x046;
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
        constexpr int NumDecimal = 0x053;
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
        bool m_bFreezeTime;
        bool m_bHideHud;
        bool m_bFreezeEffectsOnStart;
        bool m_bShowHelpOnStart;
        float m_fMinFOV;
        float m_fMaxFOV;
        float m_fZoomSpeed; // III/VC: legacy per-frame step scaled from 60 Hz; SA: degrees per second.
        float m_fMouseSensitivity;
        float m_fBaseMoveSpeed;
        float m_fFastMoveMultiplier;
        float m_fSlowMoveMultiplier;
        int m_nKeyModeToggle;
        int m_nKeyToggleTimePlayback;
        int m_nKeyWideScreen;
        int m_nKeyZoomIn;
        int m_nKeyZoomOut;
        int m_nKeyResetZoom;
        int m_nKeyShowHelp;
        int m_nKeyFreezeEffects;
        int m_nKeyCapturePhoto;
        int m_nKeyAscend;
        int m_nKeyDescend;
        int m_nKeyForward;
        int m_nKeyBackward;
        int m_nKeyLeft;
        int m_nKeyRight;
        int m_nKeyMoveFast;
        int m_nKeyMoveSlow;
        float m_fHelpFontSize;
        int m_nHoursPerPress;
        int m_nKeyHourDown;
        int m_nKeyHourUp;
    };

    enum class ModeExitReason : std::uint8_t
    {
        None,
        UserToggle,
        LoadGame,
        NewGame,
        PluginShutdown,
        EntryFailure,
        RestoreFailure
    };

    enum class ModeResource : std::uint64_t
    {
        TimeScale = 1ull << 0,
        PlayerControls = 1ull << 1,
        HudVisibility = 1ull << 2,
        HudMarkers = 1ull << 3,
        WideScreen = 1ull << 4,
        CameraFOV = 1ull << 5,
        // CameraOwnership represents an applied native camera-control operation;
        // restoration is performed with the version-specific SDK API.
        CameraOwnership = 1ull << 6,
        NativeHudPatch = 1ull << 7,
        SceneEffectsPatch = 1ull << 8,
        GameSpecificHudFlags = 1ull << 9,
        DrawFOV = 1ull << 10
    };

    // One snapshot per screenshot-mode session. Captured, applied and restored are
    // deliberately separate: a value we merely observed must not be written back,
    // and a failed restoration must remain pending for the next frame's retry.
    class ModeSnapshot
    {
    public:
        void Reset();
        void MarkCaptured(ModeResource resource);
        void MarkApplied(ModeResource resource);
        void MarkRestored(ModeResource resource);
        bool IsCaptured(ModeResource resource) const;
        bool WasApplied(ModeResource resource) const;
        bool NeedsRestore(ModeResource resource) const;
        bool HasPendingRestores() const;
        bool IsSessionTransition() const;

        ModeExitReason m_eExitReason = ModeExitReason::None;
        float m_fSavedTimeScale = 1.0f;
        int m_nSavedPlayerControlState = 0;
        bool m_bSavedHudVisible = true;
        bool m_bSavedHudMarkersVisible = true;
        bool m_bSavedWideScreen = false;
        std::array<float, 3> m_afSavedCameraFOV{};
        float m_fSavedDrawFOV = 70.0f;

        // San Andreas-specific HUD flags; left unused by III/VC adapters.
        bool m_bSavedScriptRadarHidden = false;
        bool m_bSavedAreaNameHidden = false;
        bool m_bSavedVehicleNameHidden = false;
        bool m_bSavedClockVisible = true;

        // Original instructions and per-site patch state are owned by the same
        // session snapshot as the runtime state they protect.
        std::array<std::uint8_t, 3> m_aSavedFxManagerUpdateBytes{};
        std::uint8_t m_nSavedCoronasUpdateByte = 0;
        std::uint8_t m_nSavedHudDrawOpcode = 0;
        std::uint8_t m_nSavedHudDrawAfterFadeOpcode = 0;
        bool m_bFxManagerBytesCaptured = false;
        bool m_bCoronasByteCaptured = false;
        bool m_bHudDrawByteCaptured = false;
        bool m_bHudDrawAfterFadeByteCaptured = false;
        bool m_bFxManagerPatchApplied = false;
        bool m_bCoronasPatchApplied = false;
        bool m_bHudDrawPatchApplied = false;
        bool m_bHudDrawAfterFadePatchApplied = false;

    private:
        static std::uint64_t ResourceMask(ModeResource resource);
        std::uint64_t m_nCapturedResources = 0;
        std::uint64_t m_nAppliedResources = 0;
        std::uint64_t m_nRestoredResources = 0;
    };

    class InputTracker;

    // Common lifecycle state for all game adapters. It owns the mode transition and
    // input-capture policy; adapters supply only game-specific resources/operations.
    class CameraModeController
    {
    public:
        enum class State : std::uint8_t
        {
            Inactive,
            Entering,
            Active,
            Exiting,
            RecoveryRequired
        };

        void AttachInputTracker(InputTracker* inputTracker);
        void SynchronizeInputCapture();
        bool BeginEnter();
        void MarkActive();
        bool BeginExit(ModeExitReason reason);
        void CompleteExit(bool restorationComplete);
        void RequireRecovery(ModeExitReason reason = ModeExitReason::EntryFailure);
        bool IsActive() const;
        bool IsInactive() const;
        bool NeedsRecovery() const;
        State GetState() const;
        ModeSnapshot& Snapshot();
        const ModeSnapshot& Snapshot() const;

    private:
        State m_eState = State::Inactive;
        ModeSnapshot m_Snapshot;
        InputTracker* m_pInputTracker = nullptr;
    };

    struct Vector3
    {
        float x;
        float y;
        float z;
    };

    // Shared, SDK-independent validation/math for game-world positions. The template accepts
    // CVector from any of the three Plugin-SDK targets without coupling this header to one SDK.
    template <typename TVector>
    inline bool IsUsableWorldPosition(const TVector& position)
    {
        return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
            std::fabs(position.x) < 10000.0f && std::fabs(position.y) < 10000.0f &&
            position.z > -1000.0f && position.z < 10000.0f;
    }

    template <typename TVector>
    inline float DistanceSquared(const TVector& lhs, const TVector& rhs)
    {
        const float dx = lhs.x - rhs.x;
        const float dy = lhs.y - rhs.y;
        const float dz = lhs.z - rhs.z;
        return dx * dx + dy * dy + dz * dz;
    }

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
        std::array<bool, KeyCode::StateCount> m_abPreviousKeys{};
    };

    // Shared input/environment helpers used by all three game adapters.
    bool WasDigitPressed(InputTracker& input, int digit);
    int CalculateWrappedGameHour(int currentHour, int direction, int hoursPerPress);
    void ReassertGameTimeScale(CameraModeController& controller, const InputTracker& input,
        bool timePlaybackEnabled, float& gameTimeScale);
    void ToggleGameTimePlayback(CameraModeController& controller, const InputTracker& input,
        bool& timePlaybackEnabled, float& gameTimeScale);

    // Unified camera zoom controls for GTA III, Vice City and San Andreas.
    // Configured bracket bindings remain valid; Numpad * zooms in and Numpad /
    // zooms out. Numpad + and - are reserved for changing the game hour.
    bool IsCameraZoomInDown(const InputTracker& input, const Settings& settings);
    bool IsCameraZoomOutDown(const InputTracker& input, const Settings& settings);

    Settings MakeDefaultSettings();
    void LoadSettings(Settings& settings, const wchar_t* iniFileName = L"AmazingSnapshot.ini");
    const wchar_t* GetConfigurationPath(const wchar_t* iniFileName = L"AmazingSnapshot.ini");
    std::string GetKeyLabel(int scanCode);
    // Extract the final component using either Windows path separator.
    std::string GetFileNameFromPath(const std::wstring& path);
    // Common physical-key help lines shared by all three game adapters. Labels are
    // fixed to US physical key positions and must not depend on the active layout.
    std::array<std::string, 6> BuildCommonCameraHelpLines(const Settings& settings);
    std::string BuildCameraModeHelpLine(bool freeCameraEnabled);
    std::string BuildCameraTimeHelpLine(bool freeCameraEnabled, bool timePlaybackEnabled, bool gameMenuActive);
    std::string BuildHudHelpLine(bool hudHidden);
    std::string BuildHudTimeHelpLine(bool hudHidden, bool timeFrozen);
    float ClampFloat(float value, float minimum, float maximum);
    // San Andreas: time-based FOV zoom, independent of the render frame rate.
    float UpdateContinuousCameraFOV(float currentFOV, float initialFOV, const Settings& settings,
        bool zoomInDown, bool zoomOutDown, bool resetPressed, float deltaTime);
    // GTA III/Vice City: preserve the legacy FOV step per rendered update.
    float UpdateFrameStepCameraFOV(float currentFOV, float initialFOV, const Settings& settings,
        bool zoomInDown, bool zoomOutDown, bool resetPressed);

    // Capture a RenderWare camera raster and save it as galleryN.jpg. For GTA III/VC,
    // use <game root>\userfiles\Gallery when Portable GTA is installed; otherwise
    // use Documents\<game user files>\Gallery. The camera stays opaque so adapters
    // can share this helper without exposing game-specific SDK types here.
    bool SaveRenderWareFrameToGalleryJpeg(void* renderWareCamera, const wchar_t* gameUserFilesDirectory,
        std::wstring& savedPath);

    Vector3 ForwardFromYawPitch(float yaw, float pitch);
    Vector3 RightFromYaw(float yaw);
    Vector3 Normalize(const Vector3& vector, const Vector3& fallback);
}
