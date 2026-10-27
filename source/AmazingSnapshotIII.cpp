#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "plugin.h"
#include "AmazingSnapshotCore.h"

#include "CCamera.h"
#include "CMenuManager.h"
#include "CEntity.h"
#include "common.h"
#include "CCam.h"
#include "CClock.h"
#include "CWeather.h"
#include "CHud.h"
#include "CPad.h"
#include "CTimer.h"
#include "CFont.h"
#include "CRGBA.h"
#include "RenderWare.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>


// AmazingSnapshot for GTA III PC 1.0 US / Plugin-SDK.
// Keep this file in a project built against plugin_III, not plugin_sa.
// Shared configuration/input/math and the common GTA III/VC Gallery JPEG writer live in AmazingSnapshotCore.

namespace
{
    using namespace AmazingSnapshot;

    constexpr float DEFAULT_FOV = 70.0f;
    constexpr float MIN_PITCH = -1.50f;
    constexpr float MAX_PITCH = 1.50f;
    constexpr float MAX_FRAME_DELTA = 0.05f;
    constexpr short WEATHER_COUNT_III = 4;
    constexpr short CAMERA_SWITCH_JUMPCUT = 1;

    Settings s_settings = MakeDefaultSettings();
    InputTracker s_input;

    bool s_screenshotMode = false;
    // Screenshot mode is enabled by Alt+V; V only switches this camera state.
    bool s_freeCameraEnabled = false;
    bool s_controlHelpVisible = false;
    bool s_wideScreenCurrent = false;
    bool s_timeScaleFrozen = false;
    bool s_timePlaybackEnabled = false;

    CVector s_cameraPosition(0.0f, 0.0f, 0.0f);
    CVector s_cameraForward(0.0f, 1.0f, 0.0f);
    float s_cameraYaw = 0.0f;
    float s_cameraPitch = 0.0f;
    float s_cameraFov = DEFAULT_FOV;
    float s_modeInitialFov = s_cameraFov;
    unsigned int s_lastUpdateTick = 0;

    float s_savedTimeScale = 1.0f;
    bool s_savedHudVisible = true;
    bool s_saved3DMarkersVisible = true;
    bool s_savedWideScreen = false;
    unsigned char s_savedPlayerControlState = 0;
    std::array<float, 3> s_savedCameraFov{ DEFAULT_FOV, DEFAULT_FOV, DEFAULT_FOV };
    int s_selectedWeatherPreset = -1;
    bool s_photoCaptureRequested = false;
    std::string s_photoStatus;
    DWORD s_photoStatusUntil = 0;

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
            const CVector direction = TheCamera.m_asCams[TheCamera.m_nActiveCam].m_vecFront;
            const Vector3 normalized = Normalize(
                { direction.x, direction.y, direction.z }, { 0.0f, 1.0f, 0.0f });
            return CVector(normalized.x, normalized.y, normalized.z);
        }
        const CVector direction = TheCamera.m_CameraMatrix.up;
        const Vector3 normalized = Normalize(
            { direction.x, direction.y, direction.z }, { 0.0f, 1.0f, 0.0f });
        return CVector(normalized.x, normalized.y, normalized.z);
    }

    CVector CalculateForwardVector()
    {
        const Vector3 direction = ForwardFromYawPitch(s_cameraYaw, s_cameraPitch);
        return CVector(direction.x, direction.y, direction.z);
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
        // Never accept a cached camera coordinate that is implausibly far from
        // the current player. This is the safeguard against the old random-spawn
        // symptom when a camera vector had not yet been initialized by the game.
        constexpr float MAX_INITIAL_CAMERA_DISTANCE_SQUARED = 2500.0f; // 50 game units
        const auto isNearPlayer = [&](const CVector& position)
        {
            return IsUsableWorldPosition(position) &&
                (!hasPlayer || DistanceSquared(position, playerPosition) <= MAX_INITIAL_CAMERA_DISTANCE_SQUARED);
        };

        // Prefer the actual rendered camera matrix, then the active CCam source.
        // The game-camera cache is checked last because it can be stale around a
        // camera-mode handoff. Every source must pass finite/range/player-distance
        // checks before it is allowed to seed free-camera coordinates.
        const CVector& matrixPosition = TheCamera.m_CameraMatrix.pos;
        if (isNearPlayer(matrixPosition))
            return matrixPosition;

        if (TheCamera.m_nActiveCam < 3)
        {
            const CVector& activeCameraSource = TheCamera.m_asCams[TheCamera.m_nActiveCam].m_vecSource;
            if (isNearPlayer(activeCameraSource))
                return activeCameraSource;
        }

        const CVector* gameCameraPosition = TheCamera.GetGameCamPosition();
        if (gameCameraPosition && isNearPlayer(*gameCameraPosition))
            return *gameCameraPosition;

        // Deterministic fallback: start a few units behind and slightly above
        // the player, looking in the current gameplay-camera direction.
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

        if (TheCamera.m_nActiveCam < 3)
            s_cameraFov = TheCamera.m_asCams[TheCamera.m_nActiveCam].m_fFOV;
        else
            s_cameraFov = DEFAULT_FOV;
        if (!std::isfinite(s_cameraFov) || s_cameraFov < s_settings.m_minFov || s_cameraFov > s_settings.m_maxFov)
            s_cameraFov = DEFAULT_FOV;
        s_cameraFov = ClampFloat(s_cameraFov, s_settings.m_minFov, s_settings.m_maxFov);
    }

    void ApplyFreeCamera()
    {
        if (!s_screenshotMode || !s_freeCameraEnabled || TheCamera.m_nActiveCam >= 3)
            return;

        s_cameraForward = CalculateForwardVector();
        const CVector target(
            s_cameraPosition.x + s_cameraForward.x,
            s_cameraPosition.y + s_cameraForward.y,
            s_cameraPosition.z + s_cameraForward.z);
        const CVector upOffset(0.0f, 0.0f, 1.0f);

        // Restore the working GTA III activation mechanism: TakeControlNoEntity
        // is requested once, while the native script-controlled fixed-mode state
        // and the source/target vectors are reasserted from the game and drawing
        // callbacks. The old coordinates could be garbage, so the seed position
        // is now validated against the player before this code is reached.
        TheCamera.m_nModeToGoTo = MODE_FIXED;
        TheCamera.m_nWhoIsInControlOfTheCamera = CAMCONTROL_SCRIPT;
        TheCamera.m_nTypeOfSwitch = CAMERA_SWITCH_JUMPCUT;
        TheCamera.m_bLookingAtPlayer = false;
        TheCamera.m_bLookingAtVector = true;
        TheCamera.m_nTransitionState = 0;
        TheCamera.m_nTransitionJUSTStarted = 0;

        TheCamera.m_vecFixedModeSource = s_cameraPosition;
        TheCamera.m_vecFixedModeVector = target;
        TheCamera.m_vecFixedModeUpOffSet = upOffset;
        TheCamera.SetCamPositionForFixedMode(s_cameraPosition, upOffset);

        CCam& camera = TheCamera.m_asCams[TheCamera.m_nActiveCam];
        camera.m_nCamMode = MODE_FIXED;
        camera.m_bCamLookingAtVector = true;
        camera.m_bCollisionChecksOn = false;
        camera.m_vecCamFixedModeSource = s_cameraPosition;
        camera.m_vecCamFixedModeVector = target;
        camera.m_vecCamFixedModeUpOffSet = upOffset;
        camera.m_vecSource = s_cameraPosition;
        camera.m_vecFront = s_cameraForward;
        camera.m_vecUp = upOffset;
        camera.m_fFOV = s_cameraFov;

    }

    void ApplyScreenshotFov()
    {
        if (!s_screenshotMode || TheCamera.m_nActiveCam >= 3)
            return;
        TheCamera.m_asCams[TheCamera.m_nActiveCam].m_fFOV =
            ClampFloat(s_cameraFov, s_settings.m_minFov, s_settings.m_maxFov);
    }

    void SetWideScreen(bool enabled)
    {
        s_wideScreenCurrent = enabled;
        if (enabled)
        {
            // A prior cinematic/camera state can leave a pending widescreen-off
            // request. Clear it explicitly so GTA III does not cancel this toggle
            // during its next native camera update.
            TheCamera.m_bWantsToSwitchWidescreenOff = false;
            TheCamera.SetWideScreenOn();
            TheCamera.m_bWideScreenOn = true;
            TheCamera.ProcessWideScreenOn();
        }
        else
        {
            TheCamera.SetWideScreenOff();
        }
    }

    void ToggleWideScreen()
    {
        SetWideScreen(!s_wideScreenCurrent);
    }

    void ChangeGameHour(int direction)
    {
        int hour = static_cast<int>(CClock::ms_nGameClockHours);
        hour = (hour + direction * s_settings.m_hoursPerPress) % 24;
        if (hour < 0)
            hour += 24;
        CClock::SetGameClock(static_cast<unsigned char>(hour), CClock::ms_nGameClockMinutes);
    }

    void SelectWeatherPreset(short weatherId)
    {
        if (weatherId < 0 || weatherId >= WEATHER_COUNT_III)
            return;
        CWeather::ForceWeather(weatherId);
        CWeather::ForceWeatherNow(weatherId);
        CWeather::ForcedWeatherType = weatherId;
        s_selectedWeatherPreset = weatherId;
    }

    bool WasDigitPressed(int digit)
    {
        if (digit < 0 || digit > 9)
            return false;
        const bool numberRowPressed = WasKeyPressed(KeyCode::NumberRowKey(digit));
        const bool numpadPressed = WasKeyPressed(KeyCode::NumpadKey(digit));
        return numberRowPressed || numpadPressed;
    }

    void ProcessEnvironmentHotkeys()
    {
        // Avoid consuming camera/environment shortcuts while a modifier chord is held.
        const bool modifierDown = IsKeyDown(KeyCode::Shift) || IsKeyDown(KeyCode::Control) ||
            IsKeyDown(KeyCode::Alt);

        // GTA III has four native weather presets: sunny, cloudy, rainy, foggy.
        if (!modifierDown)
        {
            for (int digit = 1; digit <= 4; ++digit)
            {
                if (WasDigitPressed(digit))
                    SelectWeatherPreset(static_cast<short>(digit - 1));
            }
        }
        else
        {
            // Keep the edge-trigger tracker synchronized while modifiers are held,
            // so a key pressed with Shift/Ctrl does not become a delayed weather press.
            for (int digit = 1; digit <= 4; ++digit)
                WasDigitPressed(digit);
        }

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
        if (!s_screenshotMode || !s_controlHelpVisible)
            return;

        const auto commonHelp = BuildCommonCameraHelpLines(s_settings);

        const float scale = s_settings.m_helpFontSize;
        const float spacing = 17.0f * (scale / 0.45f);
        const std::array<std::string, 14> lines = {{
            commonHelp[0],
            commonHelp[1],
            commonHelp[2],
            commonHelp[3],
            commonHelp[4],
            commonHelp[5],
            GetKeyLabel(s_settings.m_keyCapturePhoto) + ": save photo to Gallery",
            GetKeyLabel(s_settings.m_keyWideScreen) + ": widescreen",
            GetKeyLabel(s_settings.m_keyHourDown) + "/" + GetKeyLabel(s_settings.m_keyHourUp) + ": hour",
            "1-4: weather presets (sunny/cloudy/rain/fog)",
            BuildHudHelpLine(s_settings.m_hideHud),
            BuildCameraTimeHelpLine(s_freeCameraEnabled, s_timePlaybackEnabled, s_input.IsGameMenuInputActive()),
            GetKeyLabel(s_settings.m_keyShowHelp) + ": hide/show help",
            BuildCameraStatusLine()
        }};

        CFont::SetFontStyle(FONT_SUBTITLES);
        CFont::SetColor(CRGBA(255, 255, 255, 255));
        CFont::SetDropShadowPosition(1);
        CFont::SetBackgroundOff();
        CFont::SetPropOn();
        CFont::SetCentreOff();
        CFont::SetRightJustifyOff();
        CFont::SetWrapx(620.0f);
        for (std::size_t index = 0; index < lines.size(); ++index)
        {
            CFont::SetScale(scale, scale * 1.7f);
            CFont::PrintString(22.0f, 22.0f + static_cast<float>(index) * spacing, lines[index].c_str());
        }
    }

    bool SaveCurrentFrameAsJpeg(std::string& savedPath)
    {
        return SaveRenderWareFrameToGalleryJpeg(TheCamera.m_pRwCamera, "GTA3 User Files", savedPath);
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

        const float scale = s_settings.m_helpFontSize;
        const float spacing = 17.0f * (scale / 0.45f);
        const float y = s_controlHelpVisible ? 22.0f + 15.0f * spacing : 22.0f;
        CFont::SetFontStyle(FONT_SUBTITLES);
        CFont::SetColor(CRGBA(255, 255, 255, 255));
        CFont::SetDropShadowPosition(1);
        CFont::SetBackgroundOff();
        CFont::SetPropOn();
        CFont::SetCentreOff();
        CFont::SetRightJustifyOff();
        CFont::SetWrapx(620.0f);
        CFont::SetScale(scale, scale * 1.7f);
        CFont::PrintString(22.0f, y, s_photoStatus.c_str());
    }

    void SaveModeState()
    {
        s_savedTimeScale = CTimer::ms_fTimeScale;
        s_savedHudVisible = CHud::m_Wants_To_Draw_Hud;
        s_saved3DMarkersVisible = CHud::m_Wants_To_Draw_3dMarkers;
        s_savedWideScreen = TheCamera.m_bWideScreenOn;
        s_wideScreenCurrent = s_savedWideScreen;
        if (CPad* pad = CPad::GetPad(0))
            s_savedPlayerControlState = pad->DisablePlayerControls;
        for (int index = 0; index < 3; ++index)
            s_savedCameraFov[index] = TheCamera.m_asCams[index].m_fFOV;
    }

    void ActivateFixedCamera()
    {
        s_freeCameraEnabled = true;

        const CVector target(
            s_cameraPosition.x + s_cameraForward.x,
            s_cameraPosition.y + s_cameraForward.y,
            s_cameraPosition.z + s_cameraForward.z);
        const CVector upOffset(0.0f, 0.0f, 1.0f);

        // Request one native transition to a world-space target independent of the player.
        TheCamera.SetCamPositionForFixedMode(s_cameraPosition, upOffset);
        TheCamera.TakeControlNoEntity(target, CAMERA_SWITCH_JUMPCUT, CAMCONTROL_SCRIPT);
        TheCamera.m_bLookingAtPlayer = false;
        TheCamera.m_bLookingAtVector = true;
        TheCamera.m_vecFixedModeSource = s_cameraPosition;
        TheCamera.m_vecFixedModeVector = target;
        TheCamera.m_vecFixedModeUpOffSet = upOffset;
        ApplyFreeCamera();
    }

    void ToggleFreeCameraAttachment()
    {
        if (!s_screenshotMode)
            return;

        if (s_freeCameraEnabled)
        {
            // Keep screenshot mode, HUD settings and disabled player controls;
            // restore only the game's normal player-follow camera.
            s_freeCameraEnabled = false;
            TheCamera.RestoreWithJumpCut();
            ReassertTimeFreeze();
            s_lastUpdateTick = GetTickCount();
            return;
        }

        // Detach from the current player-follow view. Capture its current pose so
        // the camera becomes free from where the player camera was just looking.
        CaptureCameraPose();
        s_lastUpdateTick = GetTickCount();
        ActivateFixedCamera();
        // V only switches camera attachment; it must not restore the saved time
        // scale or briefly resume gameplay during the camera handoff.
        ReassertTimeFreeze();
    }

    void EnterScreenshotMode()
    {
        if (s_screenshotMode)
            return;

        PlayModeActivationSound();
        SaveModeState();
        CaptureCameraPose();
        s_modeInitialFov = s_cameraFov;
        s_screenshotMode = true;
        s_input.SetCaptureMode(true);
        s_lastUpdateTick = GetTickCount();
        s_controlHelpVisible = s_settings.m_showHelpOnStart;
        s_timeScaleFrozen = false;
        s_timePlaybackEnabled = !s_settings.m_freezeTime;
        if (s_settings.m_hideHud)
        {
            CHud::m_Wants_To_Draw_Hud = false;
            CHud::m_Wants_To_Draw_3dMarkers = false;
        }
        if (CPad* pad = CPad::GetPad(0))
            pad->DisablePlayerControls = 1;

        // Alt+V enables the whole mode and starts in free-camera state.
        // Plain V only changes the camera attachment while the mode remains on.
        ActivateFixedCamera();
        // Freeze immediately on entry when PAUSED is the selected time state.
        ReassertTimeFreeze();
    }

    void ExitScreenshotMode()
    {
        if (!s_screenshotMode)
            return;
        s_screenshotMode = false;
        s_photoCaptureRequested = false;
        s_input.SetCaptureMode(false);
        s_freeCameraEnabled = false;
        s_controlHelpVisible = false;
        s_timeScaleFrozen = false;
        s_timePlaybackEnabled = false;
        TheCamera.RestoreWithJumpCut();
        for (int index = 0; index < 3; ++index)
            TheCamera.m_asCams[index].m_fFOV = s_savedCameraFov[index];
        SetWideScreen(s_savedWideScreen);
        CHud::m_Wants_To_Draw_Hud = s_savedHudVisible;
        CHud::m_Wants_To_Draw_3dMarkers = s_saved3DMarkersVisible;
        CTimer::ms_fTimeScale = s_savedTimeScale;
        if (CPad* pad = CPad::GetPad(0))
            pad->DisablePlayerControls = s_savedPlayerControlState;
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

        // Continuous FOV zoom works in both FREE and PLAYER FOLLOW modes, just as
        // it does in San Andreas. Previously GTA III ignored zoom while following.
        const bool zoomIn = IsKeyDown(s_settings.m_keyZoomIn) || IsKeyDown(KeyCode::NumPlus);
        const bool zoomOut = IsKeyDown(s_settings.m_keyZoomOut) || IsKeyDown(KeyCode::NumMinus);
        const bool resetZoom = WasKeyPressed(s_settings.m_keyResetZoom);
        s_cameraFov = UpdateContinuousCameraFov(s_cameraFov, s_modeInitialFov,
            s_settings, zoomIn, zoomOut, resetZoom);

        if (!s_freeCameraEnabled)
        {
            s_lastUpdateTick = GetTickCount();
            return;
        }

        const unsigned int now = GetTickCount();
        float deltaTime = static_cast<float>(now - s_lastUpdateTick) / 1000.0f;
        s_lastUpdateTick = now;
        deltaTime = ClampFloat(deltaTime, 0.001f, MAX_FRAME_DELTA);

        const bool lockYaw = IsMouseButtonDown(VK_LBUTTON);
        const bool lockPitch = IsMouseButtonDown(VK_RBUTTON);
        const CMouseControllerState& mouse = CPad::NewMouseControllerState;
        if (!lockYaw) s_cameraYaw -= mouse.x * s_settings.m_mouseSensitivity;
        if (!lockPitch) s_cameraPitch += mouse.y * s_settings.m_mouseSensitivity;
        s_cameraPitch = ClampFloat(s_cameraPitch, MIN_PITCH, MAX_PITCH);

        const Vector3 forward = ForwardFromYawPitch(s_cameraYaw, s_cameraPitch);
        const Vector3 right = RightFromYaw(s_cameraYaw);
        float forwardInput = 0.0f;
        float rightInput = 0.0f;
        float verticalInput = 0.0f;
        if (IsKeyDown(s_settings.m_keyForward) || IsKeyDown(KeyCode::Up)) forwardInput += 1.0f;
        if (IsKeyDown(s_settings.m_keyBackward) || IsKeyDown(KeyCode::Down)) forwardInput -= 1.0f;
        if (IsKeyDown(s_settings.m_keyRight) || IsKeyDown(KeyCode::Right)) rightInput += 1.0f;
        if (IsKeyDown(s_settings.m_keyLeft) || IsKeyDown(KeyCode::Left)) rightInput -= 1.0f;
        if (IsKeyDown(s_settings.m_keyAscend) || IsKeyDown(KeyCode::PageUp)) verticalInput += 1.0f;
        if (IsKeyDown(s_settings.m_keyDescend) || IsKeyDown(KeyCode::PageDown)) verticalInput -= 1.0f;

        float speed = s_settings.m_baseMoveSpeed;
        if (IsKeyDown(s_settings.m_keyMoveFast)) speed *= s_settings.m_fastMoveMultiplier;
        if (IsKeyDown(s_settings.m_keyMoveSlow)) speed *= s_settings.m_slowMoveMultiplier;
        s_cameraPosition.x += (forward.x * forwardInput + right.x * rightInput) * speed * deltaTime;
        s_cameraPosition.y += (forward.y * forwardInput + right.y * rightInput) * speed * deltaTime;
        s_cameraPosition.z += (forward.z * forwardInput + verticalInput) * speed * deltaTime;
    }

    void ProcessScreenshotMode()
    {
        // Maintain game-side restrictions from gameProcessEvent. Keyboard-hook
        // installation/focus sampling and all camera input run from drawingEvent;
        // no system-wide hook stays installed during ordinary gameplay/loading.
        s_input.SetCaptureMode(s_screenshotMode);
        if (!s_screenshotMode)
            return;

        s_input.SetCaptureMode(true);
        if (s_settings.m_hideHud)
        {
            CHud::m_Wants_To_Draw_Hud = false;
            CHud::m_Wants_To_Draw_3dMarkers = false;
        }
        if (CPad* pad = CPad::GetPad(0))
            pad->DisablePlayerControls = 1;

        ReassertTimeFreeze();

    }

    void RenderScreenshotCamera()
    {
        // Match VC/SA: process hotkeys, movement and photo capture during rendering,
        // which continues while game simulation is paused.
        s_input.Initialize();
        s_input.SetGameMenuInputActive(FrontEndMenuManager.m_bMenuActive);
        ProcessScreenshotHotkeys();
        s_input.SetCaptureMode(s_screenshotMode);

        if (s_screenshotMode)
        {
            const bool helpTogglePressed = WasKeyPressed(s_settings.m_keyShowHelp);
            if (helpTogglePressed && !s_input.IsGameMenuInputActive())
                s_controlHelpVisible = !s_controlHelpVisible;
            const bool photoKeyPressed = WasKeyPressed(s_settings.m_keyCapturePhoto);
            if (photoKeyPressed && !s_input.IsGameMenuInputActive())
                s_photoCaptureRequested = true;

            if (s_settings.m_hideHud)
            {
                CHud::m_Wants_To_Draw_Hud = false;
                CHud::m_Wants_To_Draw_3dMarkers = false;
            }
            if (CPad* pad = CPad::GetPad(0))
                pad->DisablePlayerControls = 1;

            UpdateFreeCameraInput();
            ApplyFreeCamera();
            ApplyScreenshotFov();

            if (s_wideScreenCurrent)
            {
                TheCamera.m_bWantsToSwitchWidescreenOff = false;
                TheCamera.m_bWideScreenOn = true;
                TheCamera.ProcessWideScreenOn();
                TheCamera.DrawBordersForWideScreen();
            }

            // Save after camera/widescreen rendering but before drawing the help overlay.
            if (s_photoCaptureRequested)
            {
                s_photoCaptureRequested = false;
                std::string savedPath;
                if (SaveCurrentFrameAsJpeg(savedPath))
                {
                    const std::size_t separator = savedPath.find_last_of("\\/");
                    const std::string fileName = separator == std::string::npos
                        ? savedPath : savedPath.substr(separator + 1);
                    ShowPhotoStatus("Photo saved: " + fileName);
                }
                else
                {
                    ShowPhotoStatus("Photo save failed (frame read or JPEG encode/write error)");
                }
            }
        }

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
