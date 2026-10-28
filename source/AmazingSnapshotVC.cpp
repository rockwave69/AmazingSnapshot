#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "plugin.h"
#include "AmazingSnapshotCore.h"

#include "CCamera.h"
#include "CDraw.h"
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
#include "cDMAudio.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

// AmazingSnapshot for GTA Vice City PC 1.0 US / Plugin-SDK.
// Keep this file in a project built against plugin_vc, not plugin_III/plugin_sa.
// Shared configuration/input/math and the common GTA III/VC Gallery JPEG writer live in AmazingSnapshotCore.

namespace
{
    using namespace AmazingSnapshot;

    constexpr float DEFAULT_FOV = 70.0f;
    constexpr float MIN_PITCH = -1.50f;
    constexpr float MAX_PITCH = 1.50f;
    constexpr float MAX_FRAME_DELTA = 0.05f;
    constexpr short WEATHER_COUNT_VC = 6;
    constexpr short CAMERA_SWITCH_JUMPCUT = 1;
    // Vice City soundlist.h: SOUND_FRONTEND_ENTER_OR_ADJUST is sound ID 196 (0xC4).
    constexpr unsigned short SOUND_FRONTEND_ENTER_OR_ADJUST_VC = 0xC4;
    // Vice City Plugin-SDK does not expose CAMCONTROL_SCRIPT in all versions.
    // CCamera controller values follow the game convention: GAME=0, SCRIPT=1, OBBE=2.
    constexpr int CAMERA_CONTROL_SCRIPT = 1;

    Settings s_Settings = MakeDefaultSettings();
    InputTracker s_InputTracker;
    bool s_bNativePauseMenuWasActive = false;

    CameraModeController s_CameraModeController;
    bool s_bPluginInitialized = false;
    bool s_bShutdownRequested = false;
    // Screenshot mode is enabled by Alt+V; V only switches this camera state.
    bool s_bFreeCameraEnabled = false;
    bool s_bControlHelpVisible = false;
    bool s_bWideScreenCurrent = false;
    bool s_bTimePlaybackEnabled = false;

    CVector s_vecCameraPosition(0.0f, 0.0f, 0.0f);
    CVector s_vecCameraForward(0.0f, 1.0f, 0.0f);
    float s_fCameraYaw = 0.0f;
    float s_fCameraPitch = 0.0f;
    float s_fCameraFOV = DEFAULT_FOV;
    float s_fModeInitialFOV = DEFAULT_FOV;
    unsigned int s_nLastUpdateTick = 0;

    int s_nSelectedWeatherPreset = -1;
    std::string s_strPhotoStatus;
    DWORD s_nPhotoStatusUntil = 0;

    bool IsKeyDown(int key) { return s_InputTracker.IsKeyDown(key); }

    void FinalizePluginShutdown();

    void ReassertTimeFreeze()
    {
        ReassertGameTimeScale(s_CameraModeController, s_InputTracker,
            s_bTimePlaybackEnabled, CTimer::ms_fTimeScale);
    }

    void ToggleTimePlayback()
    {
        ToggleGameTimePlayback(s_CameraModeController, s_InputTracker,
            s_bTimePlaybackEnabled, CTimer::ms_fTimeScale);
    }

    bool WasKeyPressed(int key) { return s_InputTracker.WasKeyPressed(key); }
    bool IsMouseButtonDown(int virtualKey) { return s_InputTracker.IsMouseButtonDown(virtualKey); }

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
        const Vector3 direction = ForwardFromYawPitch(s_fCameraYaw, s_fCameraPitch);
        return CVector(direction.x, direction.y, direction.z);
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
        // Each source must pass finite/range/player-distance checks before it is
        // allowed to seed free-camera coordinates.
        const CVector& matrixPosition = TheCamera.m_CameraMatrix.pos;
        if (isNearPlayer(matrixPosition))
            return matrixPosition;

        if (TheCamera.m_nActiveCam < 3)
        {
            const CVector& activeCameraSource = TheCamera.m_asCams[TheCamera.m_nActiveCam].m_vecSource;
            if (isNearPlayer(activeCameraSource))
                return activeCameraSource;
        }

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
        s_vecCameraForward = GetCameraForwardFromActiveCam();
        s_vecCameraPosition = GetInitialCameraPosition();
        s_fCameraYaw = std::atan2(s_vecCameraForward.y, s_vecCameraForward.x);
        s_fCameraPitch = std::asin(ClampFloat(s_vecCameraForward.z, -1.0f, 1.0f));

        // Match the pre-refactor implementation: CDraw's FOV is the value used
        // by Vice City's renderer. Fall back to CCam only if it is invalid.
        s_fCameraFOV = CDraw::ms_fFOV;
        if (!std::isfinite(s_fCameraFOV) || s_fCameraFOV < s_Settings.m_fMinFOV || s_fCameraFOV > s_Settings.m_fMaxFOV)
        {
            s_fCameraFOV = (TheCamera.m_nActiveCam < 3)
                ? TheCamera.m_asCams[TheCamera.m_nActiveCam].m_fFOV
                : DEFAULT_FOV;
        }
        if (!std::isfinite(s_fCameraFOV) || s_fCameraFOV < s_Settings.m_fMinFOV || s_fCameraFOV > s_Settings.m_fMaxFOV)
            s_fCameraFOV = DEFAULT_FOV;
        s_fCameraFOV = ClampFloat(s_fCameraFOV, s_Settings.m_fMinFOV, s_Settings.m_fMaxFOV);
    }

    void ApplyFreeCamera()
    {
        if (!s_CameraModeController.IsActive() || !s_bFreeCameraEnabled || TheCamera.m_nActiveCam >= 3)
            return;

        s_vecCameraForward = CalculateForwardVector();
        const CVector target(
            s_vecCameraPosition.x + s_vecCameraForward.x,
            s_vecCameraPosition.y + s_vecCameraForward.y,
            s_vecCameraPosition.z + s_vecCameraForward.z);
        const CVector upOffset(0.0f, 0.0f, 1.0f);

        // Reassert Vice City's native fixed-camera state so the game camera
        // pipeline, rather than manually-written camera matrices, produces the
        // rendered view. This also keeps the free camera stable while time is frozen.
        TheCamera.m_nModeToGoTo = MODE_FIXED;
        TheCamera.m_nWhoIsInControlOfTheCamera = CAMERA_CONTROL_SCRIPT;
        TheCamera.m_nTypeOfSwitch = CAMERA_SWITCH_JUMPCUT;
        TheCamera.m_bLookingAtPlayer = false;
        TheCamera.m_bLookingAtVector = true;
        TheCamera.m_nTransitionState = 0;
        TheCamera.m_nTransitionJUSTStarted = 0;

        TheCamera.m_vecFixedModeSource = s_vecCameraPosition;
        TheCamera.m_vecFixedModeVector = target;
        TheCamera.m_vecFixedModeUpOffSet = upOffset;
        TheCamera.SetCamPositionForFixedMode(s_vecCameraPosition, upOffset);

        CCam& camera = TheCamera.m_asCams[TheCamera.m_nActiveCam];
        camera.m_nCamMode = MODE_FIXED;
        camera.m_bCamLookingAtVector = true;
        camera.m_bCollisionChecksOn = false;
        camera.m_vecCamFixedModeSource = s_vecCameraPosition;
        camera.m_vecCamFixedModeVector = target;
        camera.m_vecCamFixedModeUpOffSet = upOffset;
        camera.m_vecSource = s_vecCameraPosition;
        camera.m_vecFront = s_vecCameraForward;
        camera.m_vecUp = upOffset;
        // Restore the exact working pre-refactor path: update CDraw's projection
        // FOV, then keep the active CCam FOV synchronized with it.
        const bool drawFovChanged = CDraw::ms_fFOV != s_fCameraFOV;
        const bool cameraFovChanged = camera.m_fFOV != s_fCameraFOV;
        CDraw::SetFOV(s_fCameraFOV);
        CDraw::ms_fFOV = s_fCameraFOV;
        camera.m_fFOV = s_fCameraFOV;
        if (drawFovChanged)
            s_CameraModeController.Snapshot().MarkApplied(ModeResource::DrawFOV);
        if (cameraFovChanged)
            s_CameraModeController.Snapshot().MarkApplied(ModeResource::CameraFOV);
    }

    void ApplyScreenshotFOV()
    {
        if (!s_CameraModeController.IsActive())
            return;

        s_fCameraFOV = ClampFloat(s_fCameraFOV, s_Settings.m_fMinFOV, s_Settings.m_fMaxFOV);
        const bool drawFovChanged = CDraw::ms_fFOV != s_fCameraFOV;
        const bool cameraFovChanged = TheCamera.m_nActiveCam < 3 &&
            TheCamera.m_asCams[TheCamera.m_nActiveCam].m_fFOV != s_fCameraFOV;
        CDraw::SetFOV(s_fCameraFOV);
        CDraw::ms_fFOV = s_fCameraFOV;
        if (TheCamera.m_nActiveCam < 3)
            TheCamera.m_asCams[TheCamera.m_nActiveCam].m_fFOV = s_fCameraFOV;
        if (cameraFovChanged)
            s_CameraModeController.Snapshot().MarkApplied(ModeResource::CameraFOV);
        if (drawFovChanged)
            s_CameraModeController.Snapshot().MarkApplied(ModeResource::DrawFOV);
    }

    void SetWideScreen(bool enabled)
    {
        const bool wasWideScreenOn = TheCamera.m_bWideScreenOn;
        s_bWideScreenCurrent = enabled;
        if (enabled)
        {
            TheCamera.m_bWantsToSwitchWidescreenOff = false;
            TheCamera.SetWideScreenOn();
        }
        else
        {
            TheCamera.SetWideScreenOff();
        }

        if (s_CameraModeController.IsActive() && wasWideScreenOn != enabled)
            s_CameraModeController.Snapshot().MarkApplied(ModeResource::WideScreen);
    }
    void ToggleWideScreen()
    {
        SetWideScreen(!s_bWideScreenCurrent);
    }

    void ChangeGameHour(int direction)
    {
        const int hour = CalculateWrappedGameHour(
            static_cast<int>(CClock::ms_nGameClockHours), direction, s_Settings.m_nHoursPerPress);
        CClock::SetGameClock(static_cast<unsigned char>(hour), CClock::ms_nGameClockMinutes);
    }

    void SelectWeatherPreset(short weatherId)
    {
        if (weatherId < 0 || weatherId >= WEATHER_COUNT_VC)
            return;
        CWeather::ForceWeather(weatherId);
        CWeather::ForceWeatherNow(weatherId);
        CWeather::ForcedWeatherType = weatherId;
        s_nSelectedWeatherPreset = weatherId;
    }


    void ProcessEnvironmentHotkeys()
    {
        // Avoid consuming camera/environment shortcuts while a modifier chord is held.
        const bool modifierDown = IsKeyDown(KeyCode::Shift) || IsKeyDown(KeyCode::Control) ||
            IsKeyDown(KeyCode::Alt);

        // Vice City native weather IDs 0..5: sunny, cloudy, rainy, foggy,
        // extra sunny and hurricane. Only use those portable, known presets.
        if (!modifierDown)
        {
            for (int digit = 1; digit <= 6; ++digit)
            {
                if (WasDigitPressed(s_InputTracker, digit))
                    SelectWeatherPreset(static_cast<short>(digit - 1));
            }
        }
        else
        {
            // Keep edge tracking synchronized while modifiers are held.
            for (int digit = 1; digit <= 6; ++digit)
                WasDigitPressed(s_InputTracker, digit);
        }

        const bool hourDownPressed = WasKeyPressed(s_Settings.m_nKeyHourDown) ||
            (s_Settings.m_nKeyHourDown != KeyCode::NumMinus && WasKeyPressed(KeyCode::NumMinus));
        const bool hourUpPressed = WasKeyPressed(s_Settings.m_nKeyHourUp) ||
            (s_Settings.m_nKeyHourUp != KeyCode::NumPlus && WasKeyPressed(KeyCode::NumPlus));
        if (hourDownPressed) ChangeGameHour(-1);
        if (hourUpPressed) ChangeGameHour(1);
    }

    std::string BuildCameraStatusLine()
    {
        const int hour = static_cast<int>(CClock::ms_nGameClockHours);
        const int minute = static_cast<int>(CClock::ms_nGameClockMinutes);
        const int weather = s_nSelectedWeatherPreset >= 0
            ? s_nSelectedWeatherPreset : static_cast<int>(CWeather::ForcedWeatherType);
        const bool zoomInDown = IsCameraZoomInDown(s_InputTracker, s_Settings);
        const bool zoomOutDown = IsCameraZoomOutDown(s_InputTracker, s_Settings);
        const float activeCamFOV = TheCamera.m_nActiveCam < 3
            ? TheCamera.m_asCams[TheCamera.m_nActiveCam].m_fFOV : -1.0f;
        const float drawFOV = CDraw::ms_fFOV;
        return "FOV " + std::to_string(static_cast<int>(std::round(s_fCameraFOV))) +
            " | keys ZI:" + (zoomInDown ? "1" : "0") + " ZO:" + (zoomOutDown ? "1" : "0") +
            " | CC:" + std::to_string(static_cast<int>(std::round(activeCamFOV))) +
            " DR:" + std::to_string(static_cast<int>(std::round(drawFOV))) +
            " | time " + (hour < 10 ? "0" : "") + std::to_string(hour) + ":" +
            (minute < 10 ? "0" : "") + std::to_string(minute) +
            " | weather " + std::to_string(weather);
    }

    void DrawControlHelp()
    {
        if (!s_CameraModeController.IsActive() || !s_bControlHelpVisible || s_InputTracker.IsGameMenuInputActive())
            return;

        auto commonHelp = BuildCommonCameraHelpLines(s_Settings);
        commonHelp[5] = "Zoom: hold " + GetKeyLabel(s_Settings.m_nKeyZoomIn) + "/" +
            GetKeyLabel(s_Settings.m_nKeyZoomOut) + " or Num* in / Num/ out | " +
            GetKeyLabel(s_Settings.m_nKeyResetZoom) + ": reset zoom";

        const float scale = s_Settings.m_fHelpFontSize;
        const float spacing = 17.0f * (scale / 0.45f);
        const std::array<std::string, 14> lines = {{
            commonHelp[0],
            commonHelp[1],
            commonHelp[2],
            commonHelp[3],
            commonHelp[4],
            commonHelp[5],
            GetKeyLabel(s_Settings.m_nKeyCapturePhoto) + ": save photo to Gallery",
            GetKeyLabel(s_Settings.m_nKeyWideScreen) + ": widescreen",
            "Time hour: " + GetKeyLabel(s_Settings.m_nKeyHourDown) + "/" + GetKeyLabel(s_Settings.m_nKeyHourUp) + " or Num-/Num+",
            "1-6: weather presets (sunny/cloudy/rain/fog/extra sunny/hurricane)",
            BuildHudHelpLine(s_Settings.m_bHideHud),
            BuildCameraTimeHelpLine(s_bFreeCameraEnabled, s_bTimePlaybackEnabled, s_InputTracker.IsGameMenuInputActive()),
            GetKeyLabel(s_Settings.m_nKeyShowHelp) + ": hide/show help",
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

    bool SaveCurrentFrameAsJpeg(std::wstring& savedPath)
    {
        return SaveRenderWareFrameToGalleryJpeg(TheCamera.m_pRwCamera, L"GTA Vice City User Files", savedPath);
    }

    void ShowPhotoStatus(const std::string& message)
    {
        s_strPhotoStatus = message;
        s_nPhotoStatusUntil = GetTickCount() + 3000;
    }

    void DrawPhotoStatus()
    {
        if (s_InputTracker.IsGameMenuInputActive() || s_strPhotoStatus.empty() ||
            static_cast<LONG>(GetTickCount() - s_nPhotoStatusUntil) >= 0)
            return;

        const float scale = s_Settings.m_fHelpFontSize;
        const float spacing = 17.0f * (scale / 0.45f);
        const float y = s_bControlHelpVisible ? 22.0f + 15.0f * spacing : 22.0f;
        CFont::SetFontStyle(FONT_SUBTITLES);
        CFont::SetColor(CRGBA(255, 255, 255, 255));
        CFont::SetDropShadowPosition(1);
        CFont::SetBackgroundOff();
        CFont::SetPropOn();
        CFont::SetCentreOff();
        CFont::SetRightJustifyOff();
        CFont::SetWrapx(620.0f);
        CFont::SetScale(scale, scale * 1.7f);
        CFont::PrintString(22.0f, y, s_strPhotoStatus.c_str());
    }

    void HideInterface()
    {
        if (!s_Settings.m_bHideHud)
            return;

        ModeSnapshot& snapshot = s_CameraModeController.Snapshot();
        if (CHud::m_Wants_To_Draw_Hud)
        {
            CHud::m_Wants_To_Draw_Hud = false;
            snapshot.MarkApplied(ModeResource::HudVisibility);
        }
        if (CHud::m_Wants_To_Draw_3dMarkers)
        {
            CHud::m_Wants_To_Draw_3dMarkers = false;
            snapshot.MarkApplied(ModeResource::HudMarkers);
        }
    }

    void SaveModeState()
    {
        ModeSnapshot& snapshot = s_CameraModeController.Snapshot();
        snapshot.m_fSavedTimeScale = CTimer::ms_fTimeScale;
        snapshot.MarkCaptured(ModeResource::TimeScale);
        snapshot.m_bSavedHudVisible = CHud::m_Wants_To_Draw_Hud;
        snapshot.MarkCaptured(ModeResource::HudVisibility);
        snapshot.m_bSavedHudMarkersVisible = CHud::m_Wants_To_Draw_3dMarkers;
        snapshot.MarkCaptured(ModeResource::HudMarkers);
        snapshot.m_bSavedWideScreen = TheCamera.m_bWideScreenOn;
        snapshot.MarkCaptured(ModeResource::WideScreen);
        snapshot.m_fSavedDrawFOV = CDraw::ms_fFOV;
        if (!std::isfinite(snapshot.m_fSavedDrawFOV) || snapshot.m_fSavedDrawFOV <= 0.0f)
        {
            snapshot.m_fSavedDrawFOV = TheCamera.m_nActiveCam < 3
                ? TheCamera.m_asCams[TheCamera.m_nActiveCam].m_fFOV
                : DEFAULT_FOV;
            if (!std::isfinite(snapshot.m_fSavedDrawFOV) || snapshot.m_fSavedDrawFOV <= 0.0f)
                snapshot.m_fSavedDrawFOV = DEFAULT_FOV;
        }
        snapshot.MarkCaptured(ModeResource::DrawFOV);
        // Camera ownership is restored through the game camera API, not a copied value.
        for (int index = 0; index < 3; ++index)
            snapshot.m_afSavedCameraFOV[index] = TheCamera.m_asCams[index].m_fFOV;
        snapshot.MarkCaptured(ModeResource::CameraFOV);
        s_bWideScreenCurrent = snapshot.m_bSavedWideScreen;
        if (CPad* pad = CPad::GetPad(0))
        {
            snapshot.m_nSavedPlayerControlState = pad->DisablePlayerControls;
            snapshot.MarkCaptured(ModeResource::PlayerControls);
        }
    }

    void ActivateFixedCamera()
    {
        s_bFreeCameraEnabled = true;

        const CVector target(
            s_vecCameraPosition.x + s_vecCameraForward.x,
            s_vecCameraPosition.y + s_vecCameraForward.y,
            s_vecCameraPosition.z + s_vecCameraForward.z);
        const CVector upOffset(0.0f, 0.0f, 1.0f);

        // Request one native transition to a world-space target independent of the player.
        TheCamera.SetCamPositionForFixedMode(s_vecCameraPosition, upOffset);
        TheCamera.TakeControlNoEntity(target, CAMERA_SWITCH_JUMPCUT, CAMERA_CONTROL_SCRIPT);
        TheCamera.m_bLookingAtPlayer = false;
        TheCamera.m_bLookingAtVector = true;
        TheCamera.m_vecFixedModeSource = s_vecCameraPosition;
        TheCamera.m_vecFixedModeVector = target;
        TheCamera.m_vecFixedModeUpOffSet = upOffset;
        s_CameraModeController.Snapshot().MarkApplied(ModeResource::CameraOwnership);
        ApplyFreeCamera();
    }

    void ToggleFreeCameraAttachment()
    {
        if (!s_CameraModeController.IsActive())
            return;

        if (s_bFreeCameraEnabled)
        {
            // Keep screenshot mode, HUD settings and disabled player controls;
            // restore only the game's normal player-follow camera.
            s_bFreeCameraEnabled = false;
            TheCamera.RestoreWithJumpCut();
            s_CameraModeController.Snapshot().MarkApplied(ModeResource::CameraOwnership);
            ReassertTimeFreeze();
            s_nLastUpdateTick = GetTickCount();
            return;
        }

        // Detach from the current player-follow view. Capture its pose but preserve
        // the user's current zoom; CCam now holds the unzoomed baseline FOV.
        const float zoomFOV = s_fCameraFOV;
        CaptureCameraPose();
        s_fCameraFOV = zoomFOV;
        s_nLastUpdateTick = GetTickCount();
        ActivateFixedCamera();
        // V only switches camera attachment; it must not restore the saved time
        // scale or briefly resume gameplay during the camera handoff.
        ReassertTimeFreeze();
    }

    void ExitScreenshotMode(ModeExitReason reason = ModeExitReason::UserToggle);

    // Refuse entry during loading/transitions or while the game itself has
    // controls disabled. This is a state check only: no timer or warm-up delay.
    bool CanEnterScreenshotMode()
    {
        if (FrontEndMenuManager.m_bGameNotLoaded ||
            FrontEndMenuManager.m_bWantToLoad ||
            FrontEndMenuManager.m_bWantToRestart ||
            FrontEndMenuManager.m_bMenuActive ||
            !TheCamera.m_pRwCamera)
        {
            return false;
        }

        CPad* pad = CPad::GetPad(0);
        if (!pad || pad->DisablePlayerControls != 0)
            return false;

        return true;
    }

    void EnterScreenshotMode()
    {
        if (!CanEnterScreenshotMode())
            return;
        if (!s_CameraModeController.BeginEnter())
            return;

        SaveModeState();
        CaptureCameraPose();
        s_fModeInitialFOV = s_fCameraFOV;
        s_nLastUpdateTick = GetTickCount();
        s_bControlHelpVisible = s_Settings.m_bShowHelpOnStart;
        s_bTimePlaybackEnabled = !s_Settings.m_bFreezeTime;
        HideInterface();
        if (s_CameraModeController.NeedsRecovery())
        {
            ExitScreenshotMode(ModeExitReason::EntryFailure);
            return;
        }
        if (CPad* pad = CPad::GetPad(0))
        {
            ModeSnapshot& snapshot = s_CameraModeController.Snapshot();
            if (!snapshot.IsCaptured(ModeResource::PlayerControls))
            {
                snapshot.m_nSavedPlayerControlState = pad->DisablePlayerControls;
                snapshot.MarkCaptured(ModeResource::PlayerControls);
            }
            if (pad->DisablePlayerControls != 1)
            {
                pad->DisablePlayerControls = 1;
                snapshot.MarkApplied(ModeResource::PlayerControls);
            }
        }

        // Alt+V enables the whole mode and starts in free-camera state.
        // Plain V only changes the camera attachment while the mode remains on.
        ActivateFixedCamera();
        // Freeze immediately on entry when PAUSED is the selected time state.
        ReassertTimeFreeze();
        s_CameraModeController.MarkActive();
        if (s_CameraModeController.IsActive())
            DMAudio.PlayFrontEndSound(SOUND_FRONTEND_ENTER_OR_ADJUST_VC, 0);
    }

    void ExitScreenshotMode(ModeExitReason reason)
    {
        if (!s_CameraModeController.BeginExit(reason))
            return;

        ModeSnapshot& snapshot = s_CameraModeController.Snapshot();
        const bool sessionTransition = snapshot.IsSessionTransition();
        bool restorationComplete = true;

        // Stop acquiring input immediately. If any game resource cannot be
        // restored yet, the controller retains its snapshot and retries later.
        s_bFreeCameraEnabled = false;
        s_bControlHelpVisible = false;
        s_bTimePlaybackEnabled = false;
        // Photo capture is synchronous in RenderScreenshotCamera; there is no queued request to cancel.

        if (sessionTransition)
        {
            // The new session owns camera/HUD/timer/projection state.
            // Do not write the old session snapshot over the state the game is creating.
            snapshot.MarkRestored(ModeResource::CameraOwnership);
            snapshot.MarkRestored(ModeResource::CameraFOV);
            snapshot.MarkRestored(ModeResource::DrawFOV);
            snapshot.MarkRestored(ModeResource::WideScreen);
            // The newly loaded session owns its camera projection; discard the old baseline.
            snapshot.MarkRestored(ModeResource::HudVisibility);
            snapshot.MarkRestored(ModeResource::HudMarkers);
            snapshot.MarkRestored(ModeResource::GameSpecificHudFlags);
            snapshot.MarkRestored(ModeResource::TimeScale);
            snapshot.MarkRestored(ModeResource::PlayerControls);
        }
        else
        {
            if (snapshot.NeedsRestore(ModeResource::CameraOwnership))
            {
                TheCamera.RestoreWithJumpCut();
                snapshot.MarkRestored(ModeResource::CameraOwnership);
            }
            if (snapshot.NeedsRestore(ModeResource::CameraFOV) && snapshot.IsCaptured(ModeResource::CameraFOV))
            {
                for (int index = 0; index < 3; ++index)
                    TheCamera.m_asCams[index].m_fFOV = snapshot.m_afSavedCameraFOV[index];
                snapshot.MarkRestored(ModeResource::CameraFOV);
            }
            if (snapshot.NeedsRestore(ModeResource::DrawFOV) && snapshot.IsCaptured(ModeResource::DrawFOV))
            {
                CDraw::SetFOV(snapshot.m_fSavedDrawFOV);
                CDraw::ms_fFOV = snapshot.m_fSavedDrawFOV;
                snapshot.MarkRestored(ModeResource::DrawFOV);
            }
            if (snapshot.NeedsRestore(ModeResource::WideScreen) && snapshot.IsCaptured(ModeResource::WideScreen))
            {
                SetWideScreen(snapshot.m_bSavedWideScreen);
                snapshot.MarkRestored(ModeResource::WideScreen);
            }
            if (snapshot.NeedsRestore(ModeResource::HudVisibility) && snapshot.IsCaptured(ModeResource::HudVisibility))
            {
                CHud::m_Wants_To_Draw_Hud = snapshot.m_bSavedHudVisible;
                snapshot.MarkRestored(ModeResource::HudVisibility);
            }
            if (snapshot.NeedsRestore(ModeResource::HudMarkers) && snapshot.IsCaptured(ModeResource::HudMarkers))
            {
                CHud::m_Wants_To_Draw_3dMarkers = snapshot.m_bSavedHudMarkersVisible;
                snapshot.MarkRestored(ModeResource::HudMarkers);
            }
            if (snapshot.NeedsRestore(ModeResource::TimeScale) && snapshot.IsCaptured(ModeResource::TimeScale))
            {
                CTimer::ms_fTimeScale = snapshot.m_fSavedTimeScale;
                snapshot.MarkRestored(ModeResource::TimeScale);
            }
            if (snapshot.NeedsRestore(ModeResource::PlayerControls) && snapshot.IsCaptured(ModeResource::PlayerControls))
            {
                if (CPad* pad = CPad::GetPad(0))
                {
                    pad->DisablePlayerControls = static_cast<decltype(pad->DisablePlayerControls)>(snapshot.m_nSavedPlayerControlState);
                    snapshot.MarkRestored(ModeResource::PlayerControls);
                }
                else
                    restorationComplete = false;
            }
        }

        s_CameraModeController.CompleteExit(restorationComplete);
        if (s_CameraModeController.NeedsRecovery())
            OutputDebugStringA("AmazingSnapshot VC: mode exit incomplete; restoration will be retried.\n");
    }

    bool IsGameSessionTransitionPending()
    {
        // These flags track an explicitly requested session transition while
        // screenshot mode is active. Entry also checks m_bGameNotLoaded.
        return FrontEndMenuManager.m_bWantToRestart || FrontEndMenuManager.m_bWantToLoad;
    }

    bool ExitScreenshotModeForGameTransition()
    {
        if ((!s_CameraModeController.IsActive() && !s_CameraModeController.NeedsRecovery()) ||
            !IsGameSessionTransitionPending())
            return false;

        const ModeExitReason reason = FrontEndMenuManager.m_bWantToLoad
            ? ModeExitReason::LoadGame : ModeExitReason::NewGame;
        ExitScreenshotMode(reason);
        return true;
    }

    void ResumeScreenshotModeAfterPauseMenu()
    {
        if (!s_CameraModeController.IsActive())
            return;

        // Native pause-menu handling can replace camera ownership and clear input
        // edges. Reacquire the configured camera mode without resetting its pose.
        s_InputTracker.Reset();
        s_nLastUpdateTick = GetTickCount();
        // Release any camera ownership left by the pause frontend first. Then
        // re-request the saved fixed pose when FREE was selected; otherwise leave
        // the native player-follow camera in control.
        TheCamera.RestoreWithJumpCut();
        if (s_bFreeCameraEnabled)
            ActivateFixedCamera();

        HideInterface();
        if (CPad* pad = CPad::GetPad(0))
        {
            ModeSnapshot& snapshot = s_CameraModeController.Snapshot();
            if (!snapshot.IsCaptured(ModeResource::PlayerControls))
            {
                snapshot.m_nSavedPlayerControlState = pad->DisablePlayerControls;
                snapshot.MarkCaptured(ModeResource::PlayerControls);
            }
            if (pad->DisablePlayerControls != 1)
            {
                pad->DisablePlayerControls = 1;
                snapshot.MarkApplied(ModeResource::PlayerControls);
            }
        }

        ApplyScreenshotFOV();
        ReassertTimeFreeze();
    }

    void UpdatePauseMenuState()
    {
        const bool menuActive = FrontEndMenuManager.m_bMenuActive;
        s_InputTracker.SetGameMenuInputActive(menuActive);
        if (s_CameraModeController.IsActive() && s_bNativePauseMenuWasActive && !menuActive &&
            !IsGameSessionTransitionPending())
            ResumeScreenshotModeAfterPauseMenu();
        s_bNativePauseMenuWasActive = menuActive;
    }

    void ProcessScreenshotHotkeys()
    {
        const bool vPressed = WasKeyPressed(s_Settings.m_nKeyModeToggle);
        const bool timePlaybackPressed = WasKeyPressed(s_Settings.m_nKeyToggleTimePlayback);
        const bool altDown = IsKeyDown(KeyCode::Alt);

        if (vPressed && altDown)
        {
            // Preserve the original explicit mode-toggle shortcut.
            if (s_CameraModeController.IsActive())
                ExitScreenshotMode();
            else
                EnterScreenshotMode();
            return;
        }

        if (s_CameraModeController.IsActive() && timePlaybackPressed && !s_InputTracker.IsGameMenuInputActive())
            ToggleTimePlayback();

        if (vPressed && s_CameraModeController.IsActive() && !s_InputTracker.IsGameMenuInputActive())
        {
            ToggleFreeCameraAttachment();
            return;
        }

        // Escape is forwarded to GTA's pause/menu. Only Alt+V exits this plugin mode.
    }

    void UpdateFreeCameraInput()
    {
        if (!s_CameraModeController.IsActive())
            return;
        if (s_InputTracker.IsGameMenuInputActive())
        {
            s_nLastUpdateTick = GetTickCount();
            return;
        }

        const unsigned int now = GetTickCount();
        const float elapsedSeconds = static_cast<float>(now - s_nLastUpdateTick) / 1000.0f;
        s_nLastUpdateTick = now;
        const float deltaTime = ClampFloat(elapsedSeconds, 0.0f, MAX_FRAME_DELTA);
        ProcessEnvironmentHotkeys();
        const bool zoomIn = IsCameraZoomInDown(s_InputTracker, s_Settings);
        const bool zoomOut = IsCameraZoomOutDown(s_InputTracker, s_Settings);
        const bool resetZoom = WasKeyPressed(s_Settings.m_nKeyResetZoom);
        s_fCameraFOV = UpdateFrameStepCameraFOV(s_fCameraFOV, s_fModeInitialFOV,
            s_Settings, zoomIn, zoomOut, resetZoom);

        if (!s_bFreeCameraEnabled)
            return;

        const bool lockYaw = IsMouseButtonDown(VK_LBUTTON);
        const bool lockPitch = IsMouseButtonDown(VK_RBUTTON);
        const CMouseControllerState& mouse = CPad::NewMouseControllerState;
        if (!lockYaw) s_fCameraYaw -= mouse.x * s_Settings.m_fMouseSensitivity;
        if (!lockPitch) s_fCameraPitch += mouse.y * s_Settings.m_fMouseSensitivity;
        s_fCameraPitch = ClampFloat(s_fCameraPitch, MIN_PITCH, MAX_PITCH);

        if (WasKeyPressed(s_Settings.m_nKeyWideScreen)) ToggleWideScreen();

        const Vector3 forward = ForwardFromYawPitch(s_fCameraYaw, s_fCameraPitch);
        const Vector3 right = RightFromYaw(s_fCameraYaw);
        float forwardInput = 0.0f;
        float rightInput = 0.0f;
        float verticalInput = 0.0f;
        if (IsKeyDown(s_Settings.m_nKeyForward) || IsKeyDown(KeyCode::Up)) forwardInput += 1.0f;
        if (IsKeyDown(s_Settings.m_nKeyBackward) || IsKeyDown(KeyCode::Down)) forwardInput -= 1.0f;
        if (IsKeyDown(s_Settings.m_nKeyRight) || IsKeyDown(KeyCode::Right)) rightInput += 1.0f;
        if (IsKeyDown(s_Settings.m_nKeyLeft) || IsKeyDown(KeyCode::Left)) rightInput -= 1.0f;
        if (IsKeyDown(s_Settings.m_nKeyAscend) || IsKeyDown(KeyCode::PageUp)) verticalInput += 1.0f;
        if (IsKeyDown(s_Settings.m_nKeyDescend) || IsKeyDown(KeyCode::PageDown)) verticalInput -= 1.0f;

        float speed = s_Settings.m_fBaseMoveSpeed;
        if (IsKeyDown(s_Settings.m_nKeyMoveFast)) speed *= s_Settings.m_fFastMoveMultiplier;
        if (IsKeyDown(s_Settings.m_nKeyMoveSlow)) speed *= s_Settings.m_fSlowMoveMultiplier;
        s_vecCameraPosition.x += (forward.x * forwardInput + right.x * rightInput) * speed * deltaTime;
        s_vecCameraPosition.y += (forward.y * forwardInput + right.y * rightInput) * speed * deltaTime;
        s_vecCameraPosition.z += (forward.z * forwardInput + verticalInput) * speed * deltaTime;
    }
    void ProcessScreenshotMode()
    {
        if (!s_bPluginInitialized)
            return;
        if (s_bShutdownRequested)
        {
            if (s_CameraModeController.IsActive() || s_CameraModeController.NeedsRecovery())
                ExitScreenshotMode(ModeExitReason::PluginShutdown);
            if (!s_CameraModeController.NeedsRecovery() && s_CameraModeController.IsInactive())
                FinalizePluginShutdown();
            return;
        }
        if (ExitScreenshotModeForGameTransition())
            return;
        if (s_CameraModeController.NeedsRecovery())
        {
            ExitScreenshotMode();
            return;
        }

        // Maintain game-side restrictions here, but do not depend on this callback
        // for camera input: with FreezeTime enabled, some VC builds stop advancing
        // this path reliably while rendering continues. The global keyboard hook is
        // installed lazily only for Alt+V detection or active screenshot mode.
        s_CameraModeController.SynchronizeInputCapture();
        if (!s_CameraModeController.IsActive())
            return;

        HideInterface();
        if (s_CameraModeController.NeedsRecovery())
        {
            ExitScreenshotMode(ModeExitReason::RestoreFailure);
            return;
        }
        if (CPad* pad = CPad::GetPad(0))
        {
            ModeSnapshot& snapshot = s_CameraModeController.Snapshot();
            if (!snapshot.IsCaptured(ModeResource::PlayerControls))
            {
                snapshot.m_nSavedPlayerControlState = pad->DisablePlayerControls;
                snapshot.MarkCaptured(ModeResource::PlayerControls);
            }
            if (pad->DisablePlayerControls != 1)
            {
                pad->DisablePlayerControls = 1;
                snapshot.MarkApplied(ModeResource::PlayerControls);
            }
        }

        ReassertTimeFreeze();
        // Reapply FOV from the game-process callback too, matching the working
        // San Andreas path. This protects the chosen projection from native camera
        // updates which may run outside drawingEvent.
        if (!s_InputTracker.IsGameMenuInputActive())
            ApplyScreenshotFOV();
    }

    void RenderScreenshotCamera()
    {
        if (!s_bPluginInitialized)
            return;
        if (s_bShutdownRequested)
        {
            if (s_CameraModeController.IsActive() || s_CameraModeController.NeedsRecovery())
                ExitScreenshotMode(ModeExitReason::PluginShutdown);
            if (!s_CameraModeController.NeedsRecovery() && s_CameraModeController.IsInactive())
                FinalizePluginShutdown();
            return;
        }
        // Run the complete screenshot-mode input loop from the render callback.
        // It continues to receive keyboard input while GTA simulation time is
        // frozen, while native menu state ends keyboard pass-through on Resume.
        s_InputTracker.Initialize();
        UpdatePauseMenuState();
        if (ExitScreenshotModeForGameTransition())
            return;
        if (s_CameraModeController.NeedsRecovery())
        {
            ExitScreenshotMode();
            return;
        }
        ProcessScreenshotHotkeys();
        s_CameraModeController.SynchronizeInputCapture();

        if (s_CameraModeController.IsActive())
        {
            const bool helpTogglePressed = WasKeyPressed(s_Settings.m_nKeyShowHelp);
            if (helpTogglePressed && !s_InputTracker.IsGameMenuInputActive())
                s_bControlHelpVisible = !s_bControlHelpVisible;
            if (s_Settings.m_bHideHud)
                HideInterface();
            if (s_CameraModeController.NeedsRecovery())
            {
                ExitScreenshotMode(ModeExitReason::RestoreFailure);
                return;
            }
            if (CPad* pad = CPad::GetPad(0))
            {
                if (pad->DisablePlayerControls != 1)
                {
                    pad->DisablePlayerControls = 1;
                    s_CameraModeController.Snapshot().MarkApplied(ModeResource::PlayerControls);
                }
            }

            if (!s_InputTracker.IsGameMenuInputActive())
            {
                UpdateFreeCameraInput();
                ApplyFreeCamera();
                ApplyScreenshotFOV();

                if (s_bWideScreenCurrent)
                {
                    TheCamera.m_bWantsToSwitchWidescreenOff = false;
                    TheCamera.DrawBordersForWideScreen();
                }
            }
            else
            {
                // Do not fight GTA's native pause camera; resume from the stored pose.
                s_nLastUpdateTick = GetTickCount();
            }

            // Vice City has no San Andreas TAKE_PHOTO SCM opcode. Use the shared
            // RenderWare + GDI+ JPEG path and stock-style Gallery filenames.
            if (WasKeyPressed(s_Settings.m_nKeyCapturePhoto) && !s_InputTracker.IsGameMenuInputActive())
            {
                std::wstring savedPath;
                if (SaveCurrentFrameAsJpeg(savedPath))
                {
                    ShowPhotoStatus("Photo saved: " + GetFileNameFromPath(savedPath));
                }
                else
                {
                    ShowPhotoStatus("Photo save failed (frame read or JPEG encode/write error)");
                }
            }
        }
        // Keep time frozen even if native camera/script processing changed the scale.
        ReassertTimeFreeze();

        DrawControlHelp();
        DrawPhotoStatus();
    }

    void FinalizePluginShutdown()
    {
        if (!s_bPluginInitialized || s_CameraModeController.IsActive() || s_CameraModeController.NeedsRecovery())
            return;

        plugin::Events::gameProcessEvent -= ProcessScreenshotMode;
        plugin::Events::drawingEvent -= RenderScreenshotCamera;
        s_InputTracker.Shutdown();
        s_bPluginInitialized = false;
        s_bShutdownRequested = false;
        OutputDebugStringA("AmazingSnapshot: plugin shutdown completed.\n");
    }

    void InitializePlugin()
    {
        if (s_bPluginInitialized)
        {
            // A previous RenderWare shutdown can have deferred cleanup because a
            // game-code patch could not yet be restored. Retry before reinitializing.
            if (s_bShutdownRequested)
            {
                if (s_CameraModeController.IsActive() || s_CameraModeController.NeedsRecovery())
                    ExitScreenshotMode(ModeExitReason::PluginShutdown);
                if (!s_CameraModeController.NeedsRecovery() && s_CameraModeController.IsInactive())
                    FinalizePluginShutdown();
            }
            if (s_bPluginInitialized)
                return;
        }

        s_bShutdownRequested = false;
        s_CameraModeController.AttachInputTracker(&s_InputTracker);
        LoadSettings(s_Settings);
        s_InputTracker.SetModeToggleKey(s_Settings.m_nKeyModeToggle);
        plugin::Events::gameProcessEvent += ProcessScreenshotMode;
        plugin::Events::drawingEvent += RenderScreenshotCamera;
        s_bPluginInitialized = true;
        OutputDebugStringA("AmazingSnapshot: plugin initialized.\n");
    }

    void ShutdownPlugin()
    {
        if (!s_bPluginInitialized)
        {
            s_CameraModeController.SynchronizeInputCapture();
            s_InputTracker.Shutdown();
            return;
        }

        s_bShutdownRequested = true;
        if (s_CameraModeController.IsActive() || s_CameraModeController.NeedsRecovery())
            ExitScreenshotMode(ModeExitReason::PluginShutdown);

        s_CameraModeController.SynchronizeInputCapture();
        s_InputTracker.Shutdown();
        if (s_CameraModeController.NeedsRecovery())
        {
            // Keep runtime callbacks registered but inert to input. They only retry
            // resource restoration; they do not reinstall the global keyboard hook.
            OutputDebugStringA("AmazingSnapshot: shutdown deferred; resource restoration is still pending.\n");
            return;
        }

        FinalizePluginShutdown();
    }

    // The static registration object only wires lifecycle callbacks. Settings,
    // input-hook setup and runtime callbacks are created during initRwEvent.
    struct LifecycleRegistration
    {
        LifecycleRegistration()
        {
            plugin::Events::initRwEvent += InitializePlugin;
            plugin::Events::shutdownRwEvent += ShutdownPlugin;
        }
    } s_LifecycleRegistration;
}


BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
