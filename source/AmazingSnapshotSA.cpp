#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "plugin.h"
#include "common.h"
#include "AmazingSnapshotCore.h"
#include "CCamera.h"
#include "CMenuManager.h"
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
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <cctype>

// AmazingSnapshotSA.cpp
// GTA San Andreas 1.0 US / Plugin-SDK.
// Screenshot-camera adapter with INI-configurable controls; shared utilities live in AmazingSnapshotCore.
// The camera is driven through GTA's CCamera / CCam data and fixed-camera API.

namespace
{
    using namespace AmazingSnapshot;

    constexpr float DEFAULT_FOV = 70.0f;
    constexpr float MIN_PITCH = -1.50f;
    constexpr float MAX_PITCH = 1.50f;
    constexpr float MAX_FRAME_DELTA = 0.05f;

    Settings s_settings = MakeDefaultSettings();

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

    bool s_screenshotMode = false;
    // Alt+V controls screenshot mode; plain V only toggles camera attachment.
    bool s_freeCameraEnabled = false;
    bool s_wideScreenCurrent = false;
    bool s_timeScaleFrozen = false;
    bool s_timePlaybackEnabled = false;
    InputTracker s_input;
    int s_selectedWeatherPreset = -1;

    CVector s_cameraPosition(0.0f, 0.0f, 0.0f);
    CVector s_cameraForward(0.0f, 1.0f, 0.0f);
    float s_cameraYaw = 0.0f;
    float s_cameraPitch = 0.0f;
    float s_cameraFov = DEFAULT_FOV;
    float s_modeInitialFov = DEFAULT_FOV;
    unsigned int s_lastUpdateTick = 0;
    bool s_controlHelpVisible = false;
    bool s_suppressControlHelpForCapture = false;
    std::string s_photoStatus;
    DWORD s_photoStatusUntil = 0;
    bool s_photoCaptureRequested = false;
    bool s_sceneEffectsFrozen = false;
    bool s_sceneEffectsPatchesApplied = false;

    std::array<BYTE, 3> s_savedFxManagerUpdateBytes{};
    BYTE s_savedCoronasUpdateByte = 0;

    bool s_nativeHudDrawingSuppressed = false;
    BYTE s_savedHudDrawOpcode = 0;
    BYTE s_savedHudDrawAfterFadeOpcode = 0;

    float s_savedTimeScale = 1.0f;
    bool s_savedHudVisible = true;
    bool s_savedRadarDisplayDisabled = false;
    bool s_savedAreaNameDisplayDisabled = false;
    bool s_savedVehicleNameDisplayDisabled = false;
    bool s_savedClockVisible = true;
    bool s_saved3DMarkersVisible = true;
    bool s_savedWideScreen = false;
    unsigned short s_savedPlayerControlState = 0;
    std::array<float, 3> s_savedCameraFov{ DEFAULT_FOV, DEFAULT_FOV, DEFAULT_FOV };
    float s_savedDrawFov = DEFAULT_FOV;

    bool IsKeyDown(int key) { return s_input.IsKeyDown(key); }

    void ReassertTimeFreeze()
    {
        if (!s_screenshotMode || s_input.IsGameMenuInputActive())
            return;

        // The selected playback state is authoritative in every supported game.
        // Camera handoffs must not create an implicit unpause window.
        if (s_timePlaybackEnabled)
        {
            CTimer::ms_fTimeScale = s_savedTimeScale;
            s_timeScaleFrozen = false;
        }
        else
        {
            CTimer::ms_fTimeScale = 0.0f;
            s_timeScaleFrozen = true;
        }
    }

    void ToggleTimePlayback()
    {
        if (!s_screenshotMode || s_input.IsGameMenuInputActive())
            return;

        s_timePlaybackEnabled = !s_timePlaybackEnabled;
        // Apply the selected state immediately, even if a camera handoff is pending.
        ReassertTimeFreeze();
    }

    bool WasKeyPressed(int key) { return s_input.WasKeyPressed(key); }
    bool IsMouseButtonDown(int virtualKey) { return s_input.IsMouseButtonDown(virtualKey); }

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

    bool IsUsableWorldPosition(const CVector& position)
    {
        return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
            std::fabs(position.x) < 10000.0f && std::fabs(position.y) < 10000.0f &&
            position.z > -1000.0f && position.z < 10000.0f;
    }

    float DistanceSquared(const CVector& lhs, const CVector& rhs)
    {
        const float dx = lhs.x - rhs.x;
        const float dy = lhs.y - rhs.y;
        const float dz = lhs.z - rhs.z;
        return dx * dx + dy * dy + dz * dz;
    }

    CVector GetInitialCameraPosition()
    {
        CEntity* playerEntity = FindPlayerEntity();
        const bool hasPlayer = playerEntity != nullptr;
        const CVector playerPosition = hasPlayer
            ? playerEntity->GetPosition()
            : CVector(0.0f, 0.0f, 0.0f);
        constexpr float MAX_INITIAL_CAMERA_DISTANCE_SQUARED = 2500.0f; // 50 game units
        const auto isNearPlayer = [&](const CVector& position)
        {
            return IsUsableWorldPosition(position) &&
                (!hasPlayer || DistanceSquared(position, playerPosition) <= MAX_INITIAL_CAMERA_DISTANCE_SQUARED);
        };

        // Prefer a valid current rendered pose or active camera source. Reject
        // stale/uninitialized vectors instead of teleporting to arbitrary world
        // coordinates when the screenshot mode is toggled.
        const CVector& matrixPosition = TheCamera.m_mCameraMatrix.pos;
        if (isNearPlayer(matrixPosition))
            return matrixPosition;

        if (TheCamera.m_nActiveCam < 3)
        {
            const CVector& activeCameraSource = TheCamera.m_aCams[TheCamera.m_nActiveCam].m_vecSource;
            if (isNearPlayer(activeCameraSource))
                return activeCameraSource;
        }

        if (hasPlayer)
        {
            const CVector forward = GetCameraForwardFromActiveCam();
            CVector fallbackPosition = playerPosition;
            fallbackPosition.x -= forward.x * 4.0f;
            fallbackPosition.y -= forward.y * 4.0f;
            fallbackPosition.z += 1.5f;
            return fallbackPosition;
        }

        return IsUsableWorldPosition(matrixPosition) ? matrixPosition : CVector(0.0f, 0.0f, 10.0f);
    }

    void CaptureCameraPose()
    {
        s_cameraForward = GetCameraForwardFromActiveCam();
        s_cameraPosition = GetInitialCameraPosition();
        s_cameraYaw = std::atan2(s_cameraForward.y, s_cameraForward.x);
        s_cameraPitch = std::asin(ClampFloat(s_cameraForward.z, -1.0f, 1.0f));

        // CDraw::ms_fFOV is the projection FOV used by the renderer. Reading
        // only CCam::m_fFOV can appear to work in memory while leaving the
        // actual rendered view unchanged, so use the render FOV as the source.
        s_cameraFov = CDraw::ms_fFOV;
        if (!std::isfinite(s_cameraFov) || s_cameraFov < s_settings.m_minFov || s_cameraFov > s_settings.m_maxFov)
        {
            s_cameraFov = (TheCamera.m_nActiveCam < 3)
                ? TheCamera.m_aCams[TheCamera.m_nActiveCam].m_fFOV
                : DEFAULT_FOV;
        }

        if (!std::isfinite(s_cameraFov) || s_cameraFov < s_settings.m_minFov || s_cameraFov > s_settings.m_maxFov)
            s_cameraFov = DEFAULT_FOV;

        s_cameraFov = ClampFloat(s_cameraFov, s_settings.m_minFov, s_settings.m_maxFov);
    }

    CVector CalculateForwardVector()
    {
        const Vector3 direction = ForwardFromYawPitch(s_cameraYaw, s_cameraPitch);
        return CVector(direction.x, direction.y, direction.z);
    }

    // Forward declaration: activation applies the initial pose immediately.
    void ApplyFreeCamera();

    void ActivateFixedCamera()
    {
        s_freeCameraEnabled = true;
        s_cameraForward = CalculateForwardVector();
        const CVector lookAt(
            s_cameraPosition.x + s_cameraForward.x,
            s_cameraPosition.y + s_cameraForward.y,
            s_cameraPosition.z + s_cameraForward.z);
        const CVector upOffset(0.0f, 0.0f, 1.0f);

        TheCamera.TakeControlNoEntity(&lookAt, SWITCHTYPE_JUMPCUT, 1);
        TheCamera.m_vecFixedModeSource = s_cameraPosition;
        TheCamera.m_vecFixedModeVector = lookAt;
        TheCamera.m_vecFixedModeUpOffSet = upOffset;
        TheCamera.SetCamPositionForFixedMode(&s_cameraPosition, &upOffset);
        ApplyFreeCamera();
    }

    void ToggleFreeCameraAttachment()
    {
        if (!s_screenshotMode)
            return;

        if (s_freeCameraEnabled)
        {
            s_freeCameraEnabled = false;
            TheCamera.RestoreWithJumpCut();
            ReassertTimeFreeze();
            s_lastUpdateTick = GetTickCount();
            return;
        }
        // Start free movement at the pose currently shown by the attached player camera.
        CaptureCameraPose();
        s_lastUpdateTick = GetTickCount();
        ActivateFixedCamera();
        // Never restore the saved time scale on V. Preserve an already-established
        // freeze immediately after the native camera handoff request.
        ReassertTimeFreeze();
    }

    void ApplyFreeCamera()
    {
        if (!s_screenshotMode || !s_freeCameraEnabled || TheCamera.m_nActiveCam >= 3)
            return;

        s_cameraForward = CalculateForwardVector();
        const CVector lookAt(
            s_cameraPosition.x + s_cameraForward.x,
            s_cameraPosition.y + s_cameraForward.y,
            s_cameraPosition.z + s_cameraForward.z);
        const CVector upOffset(0.0f, 0.0f, 1.0f);

        // Update only the fixed-camera target/source consumed by GTA's own
        // CCam::Process_Fixed. Let the game's camera pipeline build the basis,
        // view matrix, inverse matrix and frustum together. Writing individual
        // m_mCameraMatrix axes here made those structures disagree and could
        // invert/distort world rendering when the mode was activated.
        TheCamera.m_vecFixedModeSource = s_cameraPosition;
        TheCamera.m_vecFixedModeVector = lookAt;
        TheCamera.m_vecFixedModeUpOffSet = upOffset;
        TheCamera.SetCamPositionForFixedMode(&s_cameraPosition, &upOffset);

        CCam& camera = TheCamera.m_aCams[TheCamera.m_nActiveCam];
        camera.m_vecCamFixedModeSource = s_cameraPosition;
        camera.m_vecCamFixedModeVector = lookAt;
        camera.m_vecCamFixedModeUpOffSet = upOffset;
        camera.m_fFOV = s_cameraFov;

        // The screenshot mode no longer patches bytes inside CCamera::Process.
        // Set the projection FOV last from the render callback instead of
        // jumping over native camera instructions at a fixed executable address.
        CDraw::SetFOV(s_cameraFov);
        CDraw::ms_fFOV = s_cameraFov;
        // Native SA camera modes can rewrite the active CCam FOV; keep both the
        // projection and the active camera field synchronized after every update.
        if (TheCamera.m_nActiveCam < 3)
            TheCamera.m_aCams[TheCamera.m_nActiveCam].m_fFOV = s_cameraFov;
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
        if (s_sceneEffectsPatchesApplied)
            return true;

        // Preserve the bytes present at runtime, including a possible compatible
        // pre-existing hook, and restore them when effect freezing is disabled.
        for (std::size_t i = 0; i < s_savedFxManagerUpdateBytes.size(); ++i)
            s_savedFxManagerUpdateBytes[i] = *reinterpret_cast<volatile BYTE*>(FX_MANAGER_UPDATE_ADDRESS + i);
        s_savedCoronasUpdateByte = *reinterpret_cast<volatile BYTE*>(CORONAS_UPDATE_ADDRESS);

        if (!WriteCodeBytes(FX_MANAGER_UPDATE_ADDRESS,
                            FX_MANAGER_RETURN_WITH_STACK_CLEANUP,
                            sizeof(FX_MANAGER_RETURN_WITH_STACK_CLEANUP)))
            return false;

        if (!WriteCodeByte(CORONAS_UPDATE_ADDRESS, NATIVE_RETURN_OPCODE))
        {
            WriteCodeBytes(FX_MANAGER_UPDATE_ADDRESS,
                           s_savedFxManagerUpdateBytes.data(),
                           s_savedFxManagerUpdateBytes.size());
            return false;
        }

        s_sceneEffectsPatchesApplied = true;
        return true;
    }

    void RestoreSceneEffectsPatches()
    {
        if (!s_sceneEffectsPatchesApplied)
        {
            s_sceneEffectsFrozen = false;
            return;
        }

        WriteCodeBytes(FX_MANAGER_UPDATE_ADDRESS,
                      s_savedFxManagerUpdateBytes.data(),
                      s_savedFxManagerUpdateBytes.size());
        WriteCodeByte(CORONAS_UPDATE_ADDRESS, s_savedCoronasUpdateByte);
        s_sceneEffectsPatchesApplied = false;
        s_sceneEffectsFrozen = false;
    }

    void SetSceneEffectsFrozen(bool freeze)
    {
        if (freeze)
            s_sceneEffectsFrozen = ApplySceneEffectsFreezePatches();
        else
            RestoreSceneEffectsPatches();
    }

    void SuppressNativeHudDrawing()
    {
        if (s_nativeHudDrawingSuppressed)
            return;

        // Save the bytes as they are at runtime so a compatible hook installed
        // by another plugin, if present, is restored rather than overwritten
        // with a hard-coded original opcode.
        s_savedHudDrawOpcode = *reinterpret_cast<volatile BYTE*>(HUD_DRAW_ADDRESS);
        s_savedHudDrawAfterFadeOpcode = *reinterpret_cast<volatile BYTE*>(HUD_DRAW_AFTER_FADE_ADDRESS);

        if (!WriteCodeByte(HUD_DRAW_ADDRESS, NATIVE_RETURN_OPCODE))
            return;

        if (!WriteCodeByte(HUD_DRAW_AFTER_FADE_ADDRESS, NATIVE_RETURN_OPCODE))
        {
            WriteCodeByte(HUD_DRAW_ADDRESS, s_savedHudDrawOpcode);
            return;
        }

        s_nativeHudDrawingSuppressed = true;
    }

    void RestoreNativeHudDrawing()
    {
        if (!s_nativeHudDrawingSuppressed)
            return;

        WriteCodeByte(HUD_DRAW_ADDRESS, s_savedHudDrawOpcode);
        WriteCodeByte(HUD_DRAW_AFTER_FADE_ADDRESS, s_savedHudDrawAfterFadeOpcode);
        s_nativeHudDrawingSuppressed = false;
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
        CHud::m_Wants_To_Draw_Hud = s_savedHudVisible;
        CHud::bScriptDontDisplayRadar = s_savedRadarDisplayDisabled;
        CHud::bScriptDontDisplayAreaName = s_savedAreaNameDisplayDisabled;
        CHud::bScriptDontDisplayVehicleName = s_savedVehicleNameDisplayDisabled;
        CHud::bDrawClock = s_savedClockVisible;
        CHud::m_bDraw3dMarkers = s_saved3DMarkersVisible;
    }

    void SaveScreenshotModeState()
    {
        s_savedTimeScale = CTimer::ms_fTimeScale;
        s_savedHudVisible = CHud::m_Wants_To_Draw_Hud;
        s_savedRadarDisplayDisabled = CHud::bScriptDontDisplayRadar;
        s_savedAreaNameDisplayDisabled = CHud::bScriptDontDisplayAreaName;
        s_savedVehicleNameDisplayDisabled = CHud::bScriptDontDisplayVehicleName;
        s_savedClockVisible = CHud::bDrawClock;
        s_saved3DMarkersVisible = CHud::m_bDraw3dMarkers;
        s_savedWideScreen = TheCamera.m_bWideScreenOn;
        s_wideScreenCurrent = s_savedWideScreen;
        s_savedDrawFov = CDraw::ms_fFOV;
        CPad* pad = CPad::GetPad(0);
        s_savedPlayerControlState = pad ? pad->DisablePlayerControls : 0;

        for (int i = 0; i < 3; ++i)
            s_savedCameraFov[i] = TheCamera.m_aCams[i].m_fFOV;

        s_modeInitialFov = s_savedDrawFov;
        if (!std::isfinite(s_modeInitialFov) || s_modeInitialFov < s_settings.m_minFov || s_modeInitialFov > s_settings.m_maxFov)
        {
            s_modeInitialFov = (TheCamera.m_nActiveCam < 3)
                ? TheCamera.m_aCams[TheCamera.m_nActiveCam].m_fFOV
                : DEFAULT_FOV;
        }
        if (!std::isfinite(s_modeInitialFov) || s_modeInitialFov < s_settings.m_minFov || s_modeInitialFov > s_settings.m_maxFov)
            s_modeInitialFov = DEFAULT_FOV;
    }

    void ChangeGameHour(int direction)
    {
        int hour = static_cast<int>(CClock::ms_nGameClockHours);
        hour = (hour + direction * s_settings.m_hoursPerPress) % 24;
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
        s_selectedWeatherPreset = weatherId;
    }

    bool WasDigitPressed(int digit)
    {
        if (digit < 0 || digit > 9)
            return false;
        const int topRowKey = KeyCode::NumberRowKey(digit);
        const int numpadKey = KeyCode::NumpadKey(digit);
        const bool topRowPressed = WasKeyPressed(topRowKey);
        const bool numpadPressed = WasKeyPressed(numpadKey);
        return topRowPressed || numpadPressed;
    }

    void ProcessWeatherHotkeys()
    {
        // Number row and numpad are both supported. Consume edge states for every
        // digit each frame, so one held key cannot leave another digit's latch stale.
        const bool shift = IsKeyDown(KeyCode::Shift);
        const bool control = IsKeyDown(KeyCode::Control);
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

        // Scan-code bindings identify the physical key. The key with '=' on an
        // unshifted layout and '+' with Shift has one scan code, so either state
        // triggers the same action. Numpad +/- remain convenient secondary keys.
        const bool hourDownPressed = WasKeyPressed(s_settings.m_keyHourDown) ||
            (s_settings.m_keyHourDown != KeyCode::NumMinus && WasKeyPressed(KeyCode::NumMinus));
        const bool hourUpPressed = WasKeyPressed(s_settings.m_keyHourUp) ||
            (s_settings.m_keyHourUp != KeyCode::NumPlus && WasKeyPressed(KeyCode::NumPlus));

        if (hourDownPressed) ChangeGameHour(-1);
        if (hourUpPressed) ChangeGameHour(1);
    }

    std::string BuildCameraStatusLine()
    {
        const int hour = static_cast<int>(CClock::ms_nGameClockHours);
        const int minute = static_cast<int>(CClock::ms_nGameClockMinutes);
        const int weather = s_selectedWeatherPreset >= 0
            ? s_selectedWeatherPreset : static_cast<int>(CWeather::ForcedWeatherType);
        return "FOV " + std::to_string(static_cast<int>(std::round(s_cameraFov))) +
            " | time " + (hour < 10 ? "0" : "") + std::to_string(hour) + ":" +
            (minute < 10 ? "0" : "") + std::to_string(minute) +
            " | weather " + std::to_string(weather);
    }

    void DrawControlHelp()
    {
        if (!s_screenshotMode || !s_controlHelpVisible || s_suppressControlHelpForCapture)
            return;

        const auto commonHelp = BuildCommonCameraHelpLines(s_settings);

        // Keep the help compact and unobtrusive. FontSize in AmazingSnapshot.ini
        // scales both axes proportionally to the default subtitle font size.
        const float fontRatio = s_settings.m_helpFontSize / 0.38f;
        const float fontScaleX = s_settings.m_helpFontSize;
        const float fontScaleY = 0.80f * fontRatio;
        const float lineSpacing = 18.0f * fontRatio;
        CFont::SetFontStyle(FONT_SUBTITLES);
        CFont::SetColor(CRGBA(255, 255, 255, 255));
        CFont::SetDropShadowPosition(1);
        CFont::SetBackground(false, false);
        CFont::SetProportional(true);
        CFont::SetOrientation(ALIGN_LEFT);
        CFont::SetWrapx(620.0f);

        const std::array<std::string, 16> lines = {{
            commonHelp[0],
            commonHelp[1],
            commonHelp[2],
            commonHelp[3],
            commonHelp[4],
            commonHelp[5],
            GetKeyLabel(s_settings.m_keyCapturePhoto) + ": save photo to Gallery",
            GetKeyLabel(s_settings.m_keyFreezeEffects) + ": effects hold " + (s_sceneEffectsFrozen ? "ON" : "OFF"),
            GetKeyLabel(s_settings.m_keyWideScreen) + ": widescreen",
            GetKeyLabel(s_settings.m_keyHourDown) + "/" + GetKeyLabel(s_settings.m_keyHourUp) + ": hour",
            "1-0: weather 0-9    Shift+1-9: weather 10-18",
            "Ctrl+1-4: weather 19-22",
            BuildHudHelpLine(s_screenshotMode && s_settings.m_hideHud),
            BuildCameraTimeHelpLine(s_freeCameraEnabled, s_timePlaybackEnabled, s_input.IsGameMenuInputActive()),
            GetKeyLabel(s_settings.m_keyShowHelp) + ": hide/show help",
            BuildCameraStatusLine()
        }};

        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            // Reapply the scale before every line. The game shares CFont state
            // globally with HUD/script text, so another renderer can overwrite
            // it between calls in some plugin combinations.
            CFont::SetScale(fontScaleX, fontScaleY);
            CFont::PrintString(22.0f, 24.0f + static_cast<float>(i) * lineSpacing, lines[i].c_str());
        }
    }

    void ShowPhotoStatus(const std::string& message)
    {
        s_photoStatus = message;
        s_photoStatusUntil = GetTickCount() + 3000;
    }

    void DrawPhotoStatus()
    {
        if (s_photoStatus.empty() || static_cast<LONG>(GetTickCount() - s_photoStatusUntil) >= 0)
            return;

        const float fontRatio = s_settings.m_helpFontSize / 0.38f;
        const float fontScaleX = s_settings.m_helpFontSize;
        const float fontScaleY = 0.80f * fontRatio;
        const float lineSpacing = 18.0f * fontRatio;
        const float y = s_controlHelpVisible ? 24.0f + 17.0f * lineSpacing : 24.0f;
        CFont::SetFontStyle(FONT_SUBTITLES);
        CFont::SetColor(CRGBA(255, 255, 255, 255));
        CFont::SetDropShadowPosition(1);
        CFont::SetBackground(false, false);
        CFont::SetProportional(true);
        CFont::SetOrientation(ALIGN_LEFT);
        CFont::SetWrapx(620.0f);
        CFont::SetScale(fontScaleX, fontScaleY);
        CFont::PrintString(22.0f, y, s_photoStatus.c_str());
    }

    // GTA SA 1.0 US stores its active gallery directory in this global character buffer.
    // This address is the same one used by portablegta.cpp (galleryPath = (char*)0xC92268).
    constexpr std::uintptr_t SA_GALLERY_PATH_ADDRESS = 0x00C92268;

    void ConfigurePortableGalleryPath()
    {
        std::string galleryPath;
        bool usingPortablePath = false;
        if (!GetGalleryDirectory("GTA San Andreas User Files", galleryPath, usingPortablePath))
            return;

        // Avoid writing outside the stock MAX_PATH-sized buffer used by this SA 1.0 US global.
        if (galleryPath.empty() || galleryPath.size() + 1 > MAX_PATH)
            return;

        char* const gameGalleryPath = reinterpret_cast<char*>(SA_GALLERY_PATH_ADDRESS);
        std::memcpy(gameGalleryPath, galleryPath.c_str(), galleryPath.size() + 1);
    }

    // Invoke the game's own TAKE_PHOTO SCM command (opcode 0x0A1E).
    // It captures the current screen without HUD and saves to the path configured above.
    void TakePhotoUsingGameSystem()
    {
        // TAKE_PHOTO can internally restore GTA's default timer scale. Preserve
        // the scale that was active immediately before invoking the SCM opcode,
        // otherwise a frozen/slow-time screenshot session starts running again.
        const float timeScaleBeforePhoto = CTimer::ms_fTimeScale;
        ConfigurePortableGalleryPath();
        static const std::array<unsigned char, 6> s_photoCommand = {
            0x1E, 0x0A, // opcode 0x0A1E: TAKE_PHOTO
            SCRIPTPARAM_STATIC_INT_8BITS, 1,
            0x00, 0x00 // harmless NOP terminator after the one command
        };

        CRunningScript script = {};
        script.m_pBaseIP = const_cast<unsigned char*>(s_photoCommand.data());
        script.m_pCurrentIP = const_cast<unsigned char*>(s_photoCommand.data());
        script.m_bIsActive = true;
        script.m_bCondResult = true;
        script.m_bIsMission = false;
        script.ProcessOneCommand();
        CTimer::ms_fTimeScale = timeScaleBeforePhoto;
    }

    void EnterScreenshotMode()
    {
        if (s_screenshotMode)
            return;

        PlayModeActivationSound();

        SaveScreenshotModeState();
        // Do not patch CCamera::Process: camera pose and projection are applied
        // through Plugin-SDK camera APIs instead of skipping native instructions.
        CaptureCameraPose();
        s_modeInitialFov = s_cameraFov; // P restores the FOV captured when screenshot mode began

        s_screenshotMode = true;
        s_input.SetCaptureMode(true);
        s_freeCameraEnabled = true;
        s_timeScaleFrozen = false;
        s_timePlaybackEnabled = !s_settings.m_freezeTime;
        s_lastUpdateTick = GetTickCount();

        // The native camera request and first fixed-camera pose are applied above.
        // ReassertTimeFreeze below now enforces the selected PAUSED/PLAYING state
        // immediately; a camera handoff no longer enables gameplay time implicitly.
        if (s_settings.m_hideHud)
            HideInterface();
        // Hold already-created FX particles and corona fading so the scene can
        // be viewed from different camera angles without short effects expiring.
        SetSceneEffectsFrozen(s_settings.m_freezeEffectsOnStart);

        if (CPad* pad = CPad::GetPad(0))
            pad->DisablePlayerControls = 1;

        // The controls panel is configurable and can be toggled with the help key.
        s_controlHelpVisible = s_settings.m_showHelpOnStart;
        ActivateFixedCamera();
        // Freeze immediately on entry when PAUSED is the selected time state.
        ReassertTimeFreeze();
    }

    void ExitScreenshotMode()
    {
        if (!s_screenshotMode)
            return;

        s_screenshotMode = false;
        s_input.SetCaptureMode(false);
        s_suppressControlHelpForCapture = false;
        s_freeCameraEnabled = false;
        s_timeScaleFrozen = false;
        s_timePlaybackEnabled = false;
        s_controlHelpVisible = false;
        s_photoCaptureRequested = false;
        RestoreSceneEffectsPatches();

        // Restore the standard game camera before returning the saved globals.
        TheCamera.RestoreWithJumpCut();
        for (int i = 0; i < 3; ++i)
            TheCamera.m_aCams[i].m_fFOV = s_savedCameraFov[i];
        CDraw::SetFOV(s_savedDrawFov);
        CDraw::ms_fFOV = s_savedDrawFov;

        if (s_savedWideScreen)
            TheCamera.SetWideScreenOn();
        else
            TheCamera.SetWideScreenOff();

        RestoreNativeHudDrawing();
        RestoreInterface();
        CTimer::ms_fTimeScale = s_savedTimeScale;

        if (CPad* pad = CPad::GetPad(0))
            pad->DisablePlayerControls = s_savedPlayerControlState;

    }

    void ToggleWideScreen()
    {
        s_wideScreenCurrent = !s_wideScreenCurrent;
        if (s_wideScreenCurrent)
            TheCamera.SetWideScreenOn();
        else
            TheCamera.SetWideScreenOff();
    }

    void UpdateZoomInput()
    {
        // Keep continuous zoom identical to the established III/VC free-camera
        // behavior. It remains available in PLAYER FOLLOW too, and does not depend
        // on the game's simulation delta while time is frozen.
        const bool zoomIn = IsKeyDown(s_settings.m_keyZoomIn) || IsKeyDown(KeyCode::NumPlus);
        const bool zoomOut = IsKeyDown(s_settings.m_keyZoomOut) || IsKeyDown(KeyCode::NumMinus);
        const bool resetPressed = WasKeyPressed(s_settings.m_keyResetZoom);
        s_cameraFov = UpdateContinuousCameraFov(s_cameraFov, s_modeInitialFov,
            s_settings, zoomIn, zoomOut, resetPressed);
    }

    void ApplyScreenshotFov()
    {
        if (!s_screenshotMode)
            return;

        s_cameraFov = ClampFloat(s_cameraFov, s_settings.m_minFov, s_settings.m_maxFov);
        // The active projection is CDraw's FOV, which native SA camera modes can
        // reset to 70 degrees. Apply projection first, then restore the active CCam
        // value as well; this prevents native camera processing from discarding a zoom step.
        CDraw::SetFOV(s_cameraFov);
        CDraw::ms_fFOV = s_cameraFov;
        if (TheCamera.m_nActiveCam < 3)
            TheCamera.m_aCams[TheCamera.m_nActiveCam].m_fFOV = s_cameraFov;
    }

    void UpdateFreeCameraInput()
    {
        if (!s_screenshotMode)
            return;
        if (s_input.IsGameMenuInputActive())
        {
            s_lastUpdateTick = GetTickCount();
            return;
        }

        ProcessEnvironmentHotkeys();
        if (WasKeyPressed(s_settings.m_keyWideScreen))
            ToggleWideScreen();

        const unsigned int now = GetTickCount();
        float deltaTime = static_cast<float>(now - s_lastUpdateTick) / 1000.0f;
        s_lastUpdateTick = now;
        deltaTime = ClampFloat(deltaTime, 0.001f, MAX_FRAME_DELTA);
        UpdateZoomInput();
        if (!s_freeCameraEnabled)
            return;

        const bool lockYaw = IsMouseButtonDown(VK_LBUTTON);   // Original script: LMB locks mouse X.
        const bool lockPitch = IsMouseButtonDown(VK_RBUTTON); // Original script: RMB locks mouse Y.

        const CMouseControllerState& mouse = CPad::NewMouseControllerState;
        // Horizontal mouse direction is reversed to correct left/right inversion.
        // Keep vertical input unchanged because the vertical axis already behaves correctly.
        if (!lockYaw)
            s_cameraYaw -= mouse.x * s_settings.m_mouseSensitivity;
        if (!lockPitch)
            s_cameraPitch += mouse.y * s_settings.m_mouseSensitivity;
        s_cameraPitch = ClampFloat(s_cameraPitch, MIN_PITCH, MAX_PITCH);

        // Move along the camera's full forward vector, including pitch. This
        // gives true 3D flight (X/Y/Z) like the III and VC adapters instead of
        // flattening forward movement onto the horizontal plane.
        const Vector3 moveForward = ForwardFromYawPitch(s_cameraYaw, s_cameraPitch);
        const Vector3 moveRight = RightFromYaw(s_cameraYaw);

        float forwardInput = 0.0f;
        float rightInput = 0.0f;
        float verticalInput = 0.0f;
        if (IsKeyDown(s_settings.m_keyForward) || IsKeyDown(KeyCode::Up)) forwardInput += 1.0f;
        if (IsKeyDown(s_settings.m_keyBackward) || IsKeyDown(KeyCode::Down)) forwardInput -= 1.0f;
        if (IsKeyDown(s_settings.m_keyRight) || IsKeyDown(KeyCode::Right)) rightInput += 1.0f;
        if (IsKeyDown(s_settings.m_keyLeft) || IsKeyDown(KeyCode::Left)) rightInput -= 1.0f;
        if (IsKeyDown(s_settings.m_keyAscend) || IsKeyDown(KeyCode::PageUp)) verticalInput += 1.0f; // Space / Page Up
        if (IsKeyDown(s_settings.m_keyDescend) || IsKeyDown(KeyCode::PageDown)) verticalInput -= 1.0f;  // C / Page Down

        float speed = s_settings.m_baseMoveSpeed;
        if (IsKeyDown(s_settings.m_keyMoveFast)) speed *= s_settings.m_fastMoveMultiplier;
        if (IsKeyDown(s_settings.m_keyMoveSlow)) speed *= s_settings.m_slowMoveMultiplier;

        s_cameraPosition.x += (moveForward.x * forwardInput + moveRight.x * rightInput) * speed * deltaTime;
        s_cameraPosition.y += (moveForward.y * forwardInput + moveRight.y * rightInput) * speed * deltaTime;
        s_cameraPosition.z += (moveForward.z * forwardInput + verticalInput) * speed * deltaTime;
        s_cameraForward = CalculateForwardVector();
    }

    void ProcessScreenshotHotkeys()
    {
        const bool vPressed = WasKeyPressed(s_settings.m_keyModeToggle);
        const bool timePlaybackPressed = WasKeyPressed(s_settings.m_keyToggleTimePlayback);
        const bool altDown = IsKeyDown(KeyCode::Alt);

        if (vPressed && altDown)
        {
            // Preserve the original explicit mode-toggle shortcut.
            if (s_screenshotMode)
                ExitScreenshotMode();
            else
                EnterScreenshotMode();
            return;
        }

        if (s_screenshotMode && timePlaybackPressed && !s_input.IsGameMenuInputActive())
            ToggleTimePlayback();

        if (vPressed && s_screenshotMode && !s_input.IsGameMenuInputActive())
        {
            ToggleFreeCameraAttachment();
            return;
        }

        // Escape is forwarded to GTA's pause/menu. Only Alt+V exits this plugin mode.
    }

    void ProcessScreenshotMode()
    {
        // Keep HUD/player restrictions maintained on the game-process path, but
        // process user input exactly once from drawingEvent. Hook installation and
        // focus sampling are frame-side; no global hook remains during normal play.
        s_input.SetCaptureMode(s_screenshotMode);
        if (!s_screenshotMode)
            return;

        s_input.SetCaptureMode(true);
        if (s_settings.m_hideHud)
            HideInterface();
        if (CPad* pad = CPad::GetPad(0))
            pad->DisablePlayerControls = 1;

        ReassertTimeFreeze();
        ApplyScreenshotFov();
    }

    void RenderScreenshotCamera()
    {
        // Match the working III/VC input path: process mode toggles and movement
        // here, so keyboard controls continue to work while simulation time is frozen.
        s_input.Initialize();
        s_input.SetGameMenuInputActive(FrontEndMenuManager.m_bMenuActive);
        ProcessScreenshotHotkeys();
        s_input.SetCaptureMode(s_screenshotMode);

        if (s_screenshotMode)
        {
            s_input.SetCaptureMode(true);
            const bool helpTogglePressed = WasKeyPressed(s_settings.m_keyShowHelp);
            if (helpTogglePressed && !s_input.IsGameMenuInputActive())
                s_controlHelpVisible = !s_controlHelpVisible;

            const bool freezeEffectsPressed = WasKeyPressed(s_settings.m_keyFreezeEffects);
            if (freezeEffectsPressed && !s_input.IsGameMenuInputActive())
                SetSceneEffectsFrozen(!s_sceneEffectsFrozen);

            // Defer capture until the camera has been updated. GTA's native
            // TAKE_PHOTO opcode saves the image to the game's Gallery.
            const bool photoKeyPressed = WasKeyPressed(s_settings.m_keyCapturePhoto);
            if (photoKeyPressed && !s_input.IsGameMenuInputActive())
                s_photoCaptureRequested = true;

            if (s_settings.m_hideHud)
                HideInterface();
            if (CPad* pad = CPad::GetPad(0))
                pad->DisablePlayerControls = 1;

            UpdateFreeCameraInput();
            ApplyFreeCamera();
            ApplyScreenshotFov();

        }

        // Process the native photo command after applying the camera and before
        // drawing hints, so the help overlay is not included in the screenshot.
        if (s_photoCaptureRequested)
        {
            s_photoCaptureRequested = false;
            // Hide the overlay for the complete native TAKE_PHOTO call as a guard
            // against any reentrant draw callback while the game captures the frame.
            s_suppressControlHelpForCapture = true;
            TakePhotoUsingGameSystem();
            s_suppressControlHelpForCapture = false;
            ShowPhotoStatus("Photo saved: GTA Gallery");
        }

        // Keep the freeze authoritative for the whole screenshot session rather
        // than applying it only on the transition into the frozen state. Native
        // photo capture and game scripts can both reset ms_fTimeScale to 1.0f.
        ReassertTimeFreeze();

        DrawControlHelp();
        DrawPhotoStatus();
    }

    void ShutdownInputTracker()
    {
        s_input.SetCaptureMode(false);
        s_input.Shutdown();
    }

    struct Bootstrap
    {
        Bootstrap()
        {
            LoadSettings(s_settings);
            s_input.SetModeToggleKey(s_settings.m_keyModeToggle);
            plugin::Events::gameProcessEvent += ProcessScreenshotMode;
            plugin::Events::drawingEvent += RenderScreenshotCamera;
            plugin::Events::shutdownRwEvent += ShutdownInputTracker;
        }
    } s_bootstrap;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
