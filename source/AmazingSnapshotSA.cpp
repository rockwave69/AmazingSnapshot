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
#include "CAudioEngine.h"
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
#include <limits>
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

    Settings s_Settings = MakeDefaultSettings();

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

    CameraModeController s_CameraModeController;
    bool s_bPluginInitialized = false;
    bool s_bShutdownRequested = false;
    // Alt+V controls screenshot mode; plain V only toggles camera attachment.
    bool s_bFreeCameraEnabled = false;
    bool s_bWideScreenCurrent = false;
    bool s_bTimePlaybackEnabled = false;
    InputTracker s_InputTracker;
    bool s_bNativePauseMenuWasActive = false;
    int s_nSelectedWeatherPreset = -1;

    CVector s_vecCameraPosition(0.0f, 0.0f, 0.0f);
    CVector s_vecCameraForward(0.0f, 1.0f, 0.0f);
    float s_fCameraYaw = 0.0f;
    float s_fCameraPitch = 0.0f;
    float s_fCameraFOV = DEFAULT_FOV;
    float s_fModeInitialFOV = DEFAULT_FOV;
    unsigned int s_nLastUpdateTick = 0;
    bool s_bControlHelpVisible = false;
    bool s_bSuppressControlHelpForCapture = false;
    std::string s_strPhotoStatus;
    DWORD s_nPhotoStatusUntil = 0;
    bool s_bPhotoCaptureRequested = false;




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
        s_vecCameraForward = GetCameraForwardFromActiveCam();
        s_vecCameraPosition = GetInitialCameraPosition();
        s_fCameraYaw = std::atan2(s_vecCameraForward.y, s_vecCameraForward.x);
        s_fCameraPitch = std::asin(ClampFloat(s_vecCameraForward.z, -1.0f, 1.0f));

        // CDraw::ms_fFOV is the projection FOV used by the renderer. Reading
        // only CCam::m_fFOV can appear to work in memory while leaving the
        // actual rendered view unchanged, so use the render FOV as the source.
        s_fCameraFOV = CDraw::ms_fFOV;
        if (!std::isfinite(s_fCameraFOV) || s_fCameraFOV < s_Settings.m_fMinFOV || s_fCameraFOV > s_Settings.m_fMaxFOV)
        {
            s_fCameraFOV = (TheCamera.m_nActiveCam < 3)
                ? TheCamera.m_aCams[TheCamera.m_nActiveCam].m_fFOV
                : DEFAULT_FOV;
        }

        if (!std::isfinite(s_fCameraFOV) || s_fCameraFOV < s_Settings.m_fMinFOV || s_fCameraFOV > s_Settings.m_fMaxFOV)
            s_fCameraFOV = DEFAULT_FOV;

        s_fCameraFOV = ClampFloat(s_fCameraFOV, s_Settings.m_fMinFOV, s_Settings.m_fMaxFOV);
    }

    CVector CalculateForwardVector()
    {
        const Vector3 direction = ForwardFromYawPitch(s_fCameraYaw, s_fCameraPitch);
        return CVector(direction.x, direction.y, direction.z);
    }

    // Forward declaration: activation applies the initial pose immediately.
    void ApplyFreeCamera();

    void ActivateFixedCamera()
    {
        s_bFreeCameraEnabled = true;
        s_vecCameraForward = CalculateForwardVector();
        const CVector lookAt(
            s_vecCameraPosition.x + s_vecCameraForward.x,
            s_vecCameraPosition.y + s_vecCameraForward.y,
            s_vecCameraPosition.z + s_vecCameraForward.z);
        const CVector upOffset(0.0f, 0.0f, 1.0f);

        TheCamera.TakeControlNoEntity(&lookAt, SWITCHTYPE_JUMPCUT, 1);
        TheCamera.m_vecFixedModeSource = s_vecCameraPosition;
        TheCamera.m_vecFixedModeVector = lookAt;
        TheCamera.m_vecFixedModeUpOffSet = upOffset;
        TheCamera.SetCamPositionForFixedMode(&s_vecCameraPosition, &upOffset);
        s_CameraModeController.Snapshot().MarkApplied(ModeResource::CameraOwnership);
        ApplyFreeCamera();
    }

    void ToggleFreeCameraAttachment()
    {
        if (!s_CameraModeController.IsActive())
            return;

        if (s_bFreeCameraEnabled)
        {
            s_bFreeCameraEnabled = false;
            TheCamera.RestoreWithJumpCut();
            s_CameraModeController.Snapshot().MarkApplied(ModeResource::CameraOwnership);
            ReassertTimeFreeze();
            s_nLastUpdateTick = GetTickCount();
            return;
        }
        // Start free movement at the pose currently shown by the attached player camera.
        CaptureCameraPose();
        s_nLastUpdateTick = GetTickCount();
        ActivateFixedCamera();
        // Never restore the saved time scale on V. Preserve an already-established
        // freeze immediately after the native camera handoff request.
        ReassertTimeFreeze();
    }

    void ApplyFreeCamera()
    {
        if (!s_CameraModeController.IsActive() || !s_bFreeCameraEnabled || TheCamera.m_nActiveCam >= 3)
            return;

        s_vecCameraForward = CalculateForwardVector();
        const CVector lookAt(
            s_vecCameraPosition.x + s_vecCameraForward.x,
            s_vecCameraPosition.y + s_vecCameraForward.y,
            s_vecCameraPosition.z + s_vecCameraForward.z);
        const CVector upOffset(0.0f, 0.0f, 1.0f);

        // Update only the fixed-camera target/source consumed by GTA's own
        // CCam::Process_Fixed. Let the game's camera pipeline build the basis,
        // view matrix, inverse matrix and frustum together. Writing individual
        // m_mCameraMatrix axes here made those structures disagree and could
        // invert/distort world rendering when the mode was activated.
        TheCamera.m_vecFixedModeSource = s_vecCameraPosition;
        TheCamera.m_vecFixedModeVector = lookAt;
        TheCamera.m_vecFixedModeUpOffSet = upOffset;
        TheCamera.SetCamPositionForFixedMode(&s_vecCameraPosition, &upOffset);

        CCam& camera = TheCamera.m_aCams[TheCamera.m_nActiveCam];
        camera.m_vecCamFixedModeSource = s_vecCameraPosition;
        camera.m_vecCamFixedModeVector = lookAt;
        camera.m_vecCamFixedModeUpOffSet = upOffset;
        camera.m_fFOV = s_fCameraFOV;

        // The screenshot mode no longer patches bytes inside CCamera::Process.
        // Set the projection FOV last from the render callback instead of
        // jumping over native camera instructions at a fixed executable address.
        const bool drawFovChanged = CDraw::ms_fFOV != s_fCameraFOV;
        const bool cameraFovChanged = TheCamera.m_nActiveCam < 3 &&
            TheCamera.m_aCams[TheCamera.m_nActiveCam].m_fFOV != s_fCameraFOV;
        CDraw::SetFOV(s_fCameraFOV);
        CDraw::ms_fFOV = s_fCameraFOV;
        // Native SA camera modes can rewrite the active CCam FOV; keep both the
        // projection and active camera field synchronized after every update.
        if (TheCamera.m_nActiveCam < 3)
            TheCamera.m_aCams[TheCamera.m_nActiveCam].m_fFOV = s_fCameraFOV;
        if (drawFovChanged)
            s_CameraModeController.Snapshot().MarkApplied(ModeResource::DrawFOV);
        if (cameraFovChanged)
            s_CameraModeController.Snapshot().MarkApplied(ModeResource::CameraFOV);
    }

    bool IsMainExecutableImageRange(std::uintptr_t address, std::size_t count)
    {
        if (address == 0 || count == 0 ||
            address > (std::numeric_limits<std::uintptr_t>::max)() - count)
        {
            return false;
        }

        const HMODULE executableModule = GetModuleHandleW(nullptr);
        if (!executableModule)
            return false;

        const std::uintptr_t imageBase = reinterpret_cast<std::uintptr_t>(executableModule);
        const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0)
            return false;

        const std::uintptr_t ntHeaderAddress = imageBase + static_cast<std::uintptr_t>(dosHeader->e_lfanew);
        if (ntHeaderAddress < imageBase ||
            ntHeaderAddress > (std::numeric_limits<std::uintptr_t>::max)() - sizeof(IMAGE_NT_HEADERS32))
        {
            return false;
        }

        const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS32*>(ntHeaderAddress);
        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
            ntHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            return false;
        }

        const std::uintptr_t imageSize = ntHeaders->OptionalHeader.SizeOfImage;
        if (imageSize == 0 || imageBase > (std::numeric_limits<std::uintptr_t>::max)() - imageSize)
            return false;

        const std::uintptr_t imageEnd = imageBase + imageSize;
        return address >= imageBase && address + count <= imageEnd;
    }

    bool IsExecutableCodeRange(std::uintptr_t address, std::size_t count)
    {
        if (!IsMainExecutableImageRange(address, count))
            return false;

        MEMORY_BASIC_INFORMATION memoryInfo{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &memoryInfo, sizeof(memoryInfo)) != sizeof(memoryInfo))
            return false;
        if (memoryInfo.State != MEM_COMMIT || (memoryInfo.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;

        const DWORD baseProtection = memoryInfo.Protect & 0xFF;
        const bool executable = baseProtection == PAGE_EXECUTE ||
            baseProtection == PAGE_EXECUTE_READ ||
            baseProtection == PAGE_EXECUTE_READWRITE ||
            baseProtection == PAGE_EXECUTE_WRITECOPY;
        const std::uintptr_t regionStart = reinterpret_cast<std::uintptr_t>(memoryInfo.BaseAddress);
        const std::uintptr_t regionEnd = regionStart + memoryInfo.RegionSize;
        return executable && address >= regionStart && address + count <= regionEnd;
    }

    bool CodeBytesMatch(std::uintptr_t address, const BYTE* expected, std::size_t count)
    {
        if (!expected || !IsExecutableCodeRange(address, count))
            return false;
        const volatile BYTE* current = reinterpret_cast<const volatile BYTE*>(address);
        for (std::size_t i = 0; i < count; ++i)
        {
            if (current[i] != expected[i])
                return false;
        }
        return true;
    }

    bool WriteCodeBytes(std::uintptr_t address, const BYTE* values, std::size_t count,
        bool* bytesWritten = nullptr)
    {
        if (bytesWritten)
            *bytesWritten = false;
        if (!values || !IsExecutableCodeRange(address, count))
            return false;

        void* const target = reinterpret_cast<void*>(address);
        DWORD oldProtection = 0;
        if (!VirtualProtect(target, count, PAGE_EXECUTE_READWRITE, &oldProtection))
            return false;

        volatile BYTE* destination = reinterpret_cast<volatile BYTE*>(target);
        for (std::size_t i = 0; i < count; ++i)
            destination[i] = values[i];
        if (bytesWritten)
            *bytesWritten = true;

        const BOOL cacheFlushed = FlushInstructionCache(GetCurrentProcess(), target, count);
        DWORD ignoredProtection = 0;
        const BOOL protectionRestored = VirtualProtect(target, count, oldProtection, &ignoredProtection);
        return cacheFlushed != FALSE && protectionRestored != FALSE;
    }

    bool WriteCodeByte(std::uintptr_t address, BYTE value, bool* bytesWritten = nullptr)
    {
        return WriteCodeBytes(address, &value, sizeof(value), bytesWritten);
    }

    bool HasPendingSceneEffectsPatches(const ModeSnapshot& snapshot)
    {
        return snapshot.m_bFxManagerPatchApplied || snapshot.m_bCoronasPatchApplied;
    }

    bool ApplySceneEffectsFreezePatches()
    {
        ModeSnapshot& snapshot = s_CameraModeController.Snapshot();
        if (snapshot.m_bFxManagerPatchApplied && snapshot.m_bCoronasPatchApplied)
            return true;

        if (!IsExecutableCodeRange(FX_MANAGER_UPDATE_ADDRESS, sizeof(FX_MANAGER_RETURN_WITH_STACK_CLEANUP)) ||
            !IsExecutableCodeRange(CORONAS_UPDATE_ADDRESS, sizeof(BYTE)))
        {
            OutputDebugStringA("AmazingSnapshot SA: effect patch refused; target is not executable memory.\n");
            return false;
        }

        // Capture original code only once per session. Never adopt an already
        // patched RET as the original bytes, and never overwrite a non-active site
        // if another mod changed it since we captured the baseline.
        if (!snapshot.m_bFxManagerBytesCaptured)
        {
            std::array<BYTE, sizeof(FX_MANAGER_RETURN_WITH_STACK_CLEANUP)> currentBytes{};
            for (std::size_t i = 0; i < currentBytes.size(); ++i)
                currentBytes[i] = *reinterpret_cast<volatile BYTE*>(FX_MANAGER_UPDATE_ADDRESS + i);
            if (std::equal(currentBytes.begin(), currentBytes.end(),
                FX_MANAGER_RETURN_WITH_STACK_CLEANUP))
            {
                OutputDebugStringA("AmazingSnapshot SA: refusing to capture an already patched FX entry point.\n");
                return false;
            }
            snapshot.m_aSavedFxManagerUpdateBytes = currentBytes;
            snapshot.m_bFxManagerBytesCaptured = true;
            snapshot.MarkCaptured(ModeResource::SceneEffectsPatch);
        }
        if (!snapshot.m_bCoronasByteCaptured)
        {
            const BYTE currentOpcode = *reinterpret_cast<volatile BYTE*>(CORONAS_UPDATE_ADDRESS);
            if (currentOpcode == NATIVE_RETURN_OPCODE)
            {
                OutputDebugStringA("AmazingSnapshot SA: refusing to capture an already patched corona entry point.\n");
                return false;
            }
            snapshot.m_nSavedCoronasUpdateByte = currentOpcode;
            snapshot.m_bCoronasByteCaptured = true;
            snapshot.MarkCaptured(ModeResource::SceneEffectsPatch);
        }

        if ((!snapshot.m_bFxManagerPatchApplied &&
                !CodeBytesMatch(FX_MANAGER_UPDATE_ADDRESS, snapshot.m_aSavedFxManagerUpdateBytes.data(),
                    snapshot.m_aSavedFxManagerUpdateBytes.size())) ||
            (!snapshot.m_bCoronasPatchApplied &&
                !CodeBytesMatch(CORONAS_UPDATE_ADDRESS, &snapshot.m_nSavedCoronasUpdateByte, sizeof(BYTE))))
        {
            OutputDebugStringA("AmazingSnapshot SA: effect patch refused; original code changed after capture.\n");
            return false;
        }

        bool bytesWritten = false;
        const bool fxWriteSucceeded = WriteCodeBytes(FX_MANAGER_UPDATE_ADDRESS,
            FX_MANAGER_RETURN_WITH_STACK_CLEANUP, sizeof(FX_MANAGER_RETURN_WITH_STACK_CLEANUP), &bytesWritten);
        if (bytesWritten)
        {
            snapshot.m_bFxManagerPatchApplied = true;
            snapshot.MarkApplied(ModeResource::SceneEffectsPatch);
        }
        if (!fxWriteSucceeded)
        {
            return false;
        }

        bytesWritten = false;
        const bool coronasWriteSucceeded = WriteCodeByte(CORONAS_UPDATE_ADDRESS, NATIVE_RETURN_OPCODE, &bytesWritten);
        if (bytesWritten)
        {
            snapshot.m_bCoronasPatchApplied = true;
            snapshot.MarkApplied(ModeResource::SceneEffectsPatch);
        }
        if (!coronasWriteSucceeded)
            return false;

        return true;
    }

    bool RestoreSceneEffectsPatches()
    {
        ModeSnapshot& snapshot = s_CameraModeController.Snapshot();
        bool restored = true;

        if (snapshot.m_bFxManagerPatchApplied)
        {
            bool bytesWritten = false;
            const bool success = snapshot.m_bFxManagerBytesCaptured &&
                WriteCodeBytes(FX_MANAGER_UPDATE_ADDRESS, snapshot.m_aSavedFxManagerUpdateBytes.data(),
                    snapshot.m_aSavedFxManagerUpdateBytes.size(), &bytesWritten);
            if (success && bytesWritten)
                snapshot.m_bFxManagerPatchApplied = false;
            else
                restored = false;
        }
        if (snapshot.m_bCoronasPatchApplied)
        {
            bool bytesWritten = false;
            const bool success = snapshot.m_bCoronasByteCaptured &&
                WriteCodeByte(CORONAS_UPDATE_ADDRESS, snapshot.m_nSavedCoronasUpdateByte, &bytesWritten);
            if (success && bytesWritten)
                snapshot.m_bCoronasPatchApplied = false;
            else
                restored = false;
        }

        const bool patchesRemain = HasPendingSceneEffectsPatches(snapshot);
        if (restored && !patchesRemain)
            snapshot.MarkRestored(ModeResource::SceneEffectsPatch);
        else
            OutputDebugStringA("AmazingSnapshot SA: effect patch restore failed; retry remains pending.\n");
        return restored && !patchesRemain;
    }

    void SetSceneEffectsFrozen(bool freeze)
    {
        if (freeze)
        {
            const bool applied = ApplySceneEffectsFreezePatches();
            if (!applied && HasPendingSceneEffectsPatches(s_CameraModeController.Snapshot()) &&
                !RestoreSceneEffectsPatches())
            {
                s_CameraModeController.RequireRecovery(ModeExitReason::EntryFailure);
            }
        }
        else
        {
            if (!RestoreSceneEffectsPatches())
                s_CameraModeController.RequireRecovery(ModeExitReason::RestoreFailure);
        }
    }

    bool HasPendingNativeHudPatches(const ModeSnapshot& snapshot)
    {
        return snapshot.m_bHudDrawPatchApplied || snapshot.m_bHudDrawAfterFadePatchApplied;
    }

    bool SuppressNativeHudDrawing()
    {
        ModeSnapshot& snapshot = s_CameraModeController.Snapshot();
        if (snapshot.m_bHudDrawPatchApplied && snapshot.m_bHudDrawAfterFadePatchApplied)
            return true;
        if (!IsExecutableCodeRange(HUD_DRAW_ADDRESS, sizeof(BYTE)) ||
            !IsExecutableCodeRange(HUD_DRAW_AFTER_FADE_ADDRESS, sizeof(BYTE)))
        {
            OutputDebugStringA("AmazingSnapshot SA: HUD patch refused; target is not executable memory.\n");
            return false;
        }

        if (!snapshot.m_bHudDrawByteCaptured)
        {
            const BYTE currentOpcode = *reinterpret_cast<volatile BYTE*>(HUD_DRAW_ADDRESS);
            if (currentOpcode == NATIVE_RETURN_OPCODE)
            {
                OutputDebugStringA("AmazingSnapshot SA: refusing to capture an already patched HUD entry point.\n");
                return false;
            }
            snapshot.m_nSavedHudDrawOpcode = currentOpcode;
            snapshot.m_bHudDrawByteCaptured = true;
            snapshot.MarkCaptured(ModeResource::NativeHudPatch);
        }
        if (!snapshot.m_bHudDrawAfterFadeByteCaptured)
        {
            const BYTE currentOpcode = *reinterpret_cast<volatile BYTE*>(HUD_DRAW_AFTER_FADE_ADDRESS);
            if (currentOpcode == NATIVE_RETURN_OPCODE)
            {
                OutputDebugStringA("AmazingSnapshot SA: refusing to capture an already patched HUD fade entry point.\n");
                return false;
            }
            snapshot.m_nSavedHudDrawAfterFadeOpcode = currentOpcode;
            snapshot.m_bHudDrawAfterFadeByteCaptured = true;
            snapshot.MarkCaptured(ModeResource::NativeHudPatch);
        }

        if ((!snapshot.m_bHudDrawPatchApplied &&
                !CodeBytesMatch(HUD_DRAW_ADDRESS, &snapshot.m_nSavedHudDrawOpcode, sizeof(BYTE))) ||
            (!snapshot.m_bHudDrawAfterFadePatchApplied &&
                !CodeBytesMatch(HUD_DRAW_AFTER_FADE_ADDRESS, &snapshot.m_nSavedHudDrawAfterFadeOpcode, sizeof(BYTE))))
        {
            OutputDebugStringA("AmazingSnapshot SA: HUD patch refused; original code changed after capture.\n");
            return false;
        }

        bool bytesWritten = false;
        const bool hudWriteSucceeded = WriteCodeByte(HUD_DRAW_ADDRESS, NATIVE_RETURN_OPCODE, &bytesWritten);
        if (bytesWritten)
        {
            snapshot.m_bHudDrawPatchApplied = true;
            snapshot.MarkApplied(ModeResource::NativeHudPatch);
        }
        if (!hudWriteSucceeded)
        {
            return false;
        }

        bytesWritten = false;
        const bool fadeWriteSucceeded = WriteCodeByte(HUD_DRAW_AFTER_FADE_ADDRESS,
            NATIVE_RETURN_OPCODE, &bytesWritten);
        if (bytesWritten)
        {
            snapshot.m_bHudDrawAfterFadePatchApplied = true;
            snapshot.MarkApplied(ModeResource::NativeHudPatch);
        }
        if (!fadeWriteSucceeded)
            return false;
        return true;
    }

    bool RestoreNativeHudDrawing()
    {
        ModeSnapshot& snapshot = s_CameraModeController.Snapshot();
        bool restored = true;
        if (snapshot.m_bHudDrawPatchApplied)
        {
            bool bytesWritten = false;
            const bool success = snapshot.m_bHudDrawByteCaptured &&
                WriteCodeByte(HUD_DRAW_ADDRESS, snapshot.m_nSavedHudDrawOpcode, &bytesWritten);
            if (success && bytesWritten)
                snapshot.m_bHudDrawPatchApplied = false;
            else
                restored = false;
        }
        if (snapshot.m_bHudDrawAfterFadePatchApplied)
        {
            bool bytesWritten = false;
            const bool success = snapshot.m_bHudDrawAfterFadeByteCaptured &&
                WriteCodeByte(HUD_DRAW_AFTER_FADE_ADDRESS, snapshot.m_nSavedHudDrawAfterFadeOpcode, &bytesWritten);
            if (success && bytesWritten)
                snapshot.m_bHudDrawAfterFadePatchApplied = false;
            else
                restored = false;
        }

        const bool patchesRemain = HasPendingNativeHudPatches(snapshot);
        if (restored && !patchesRemain)
            snapshot.MarkRestored(ModeResource::NativeHudPatch);
        else
            OutputDebugStringA("AmazingSnapshot SA: HUD patch restore failed; retry remains pending.\n");
        return restored && !patchesRemain;
    }

    void HideInterface()
    {
        // m_Wants_To_Draw_Hud and the script flags hide the standard elements;
        // temporarily bypassing both native HUD render passes also suppresses
        // overlays/messages drawn outside those individual flags.
        const bool nativeHudSuppressed = SuppressNativeHudDrawing();
        if (!nativeHudSuppressed && HasPendingNativeHudPatches(s_CameraModeController.Snapshot()) &&
            !RestoreNativeHudDrawing())
        {
            s_CameraModeController.RequireRecovery(ModeExitReason::EntryFailure);
        }
        ModeSnapshot& snapshot = s_CameraModeController.Snapshot();
        if (CHud::m_Wants_To_Draw_Hud)
        {
            CHud::m_Wants_To_Draw_Hud = false;
            snapshot.MarkApplied(ModeResource::HudVisibility);
        }

        bool gameHudFlagsChanged = false;
        if (!CHud::bScriptDontDisplayRadar) { CHud::bScriptDontDisplayRadar = true; gameHudFlagsChanged = true; }
        if (!CHud::bScriptDontDisplayAreaName) { CHud::bScriptDontDisplayAreaName = true; gameHudFlagsChanged = true; }
        if (!CHud::bScriptDontDisplayVehicleName) { CHud::bScriptDontDisplayVehicleName = true; gameHudFlagsChanged = true; }
        if (CHud::bDrawClock) { CHud::bDrawClock = false; gameHudFlagsChanged = true; }
        if (gameHudFlagsChanged)
            snapshot.MarkApplied(ModeResource::GameSpecificHudFlags);

        if (CHud::m_bDraw3dMarkers)
        {
            CHud::m_bDraw3dMarkers = false;
            snapshot.MarkApplied(ModeResource::HudMarkers);
        }
    }

    void SaveScreenshotModeState()
    {
        ModeSnapshot& snapshot = s_CameraModeController.Snapshot();
        snapshot.m_fSavedTimeScale = CTimer::ms_fTimeScale;
        snapshot.MarkCaptured(ModeResource::TimeScale);
        snapshot.m_bSavedHudVisible = CHud::m_Wants_To_Draw_Hud;
        snapshot.MarkCaptured(ModeResource::HudVisibility);
        snapshot.m_bSavedHudMarkersVisible = CHud::m_bDraw3dMarkers;
        snapshot.MarkCaptured(ModeResource::HudMarkers);
        snapshot.m_bSavedWideScreen = TheCamera.m_bWideScreenOn;
        snapshot.MarkCaptured(ModeResource::WideScreen);
        snapshot.m_fSavedDrawFOV = CDraw::ms_fFOV;
        if (!std::isfinite(snapshot.m_fSavedDrawFOV) || snapshot.m_fSavedDrawFOV <= 0.0f)
        {
            snapshot.m_fSavedDrawFOV = TheCamera.m_nActiveCam < 3
                ? TheCamera.m_aCams[TheCamera.m_nActiveCam].m_fFOV
                : DEFAULT_FOV;
            if (!std::isfinite(snapshot.m_fSavedDrawFOV) || snapshot.m_fSavedDrawFOV <= 0.0f)
                snapshot.m_fSavedDrawFOV = DEFAULT_FOV;
        }
        snapshot.MarkCaptured(ModeResource::DrawFOV);
        // Camera ownership is restored through the game camera API, not a copied value.
        for (int i = 0; i < 3; ++i)
            snapshot.m_afSavedCameraFOV[i] = TheCamera.m_aCams[i].m_fFOV;
        snapshot.MarkCaptured(ModeResource::CameraFOV);

        snapshot.m_bSavedScriptRadarHidden = CHud::bScriptDontDisplayRadar;
        snapshot.m_bSavedAreaNameHidden = CHud::bScriptDontDisplayAreaName;
        snapshot.m_bSavedVehicleNameHidden = CHud::bScriptDontDisplayVehicleName;
        snapshot.m_bSavedClockVisible = CHud::bDrawClock;
        snapshot.MarkCaptured(ModeResource::GameSpecificHudFlags);

        s_bWideScreenCurrent = snapshot.m_bSavedWideScreen;
        CPad* pad = CPad::GetPad(0);
        if (pad)
        {
            snapshot.m_nSavedPlayerControlState = pad->DisablePlayerControls;
            snapshot.MarkCaptured(ModeResource::PlayerControls);
        }

        s_fModeInitialFOV = snapshot.m_fSavedDrawFOV;
        if (!std::isfinite(s_fModeInitialFOV) || s_fModeInitialFOV < s_Settings.m_fMinFOV || s_fModeInitialFOV > s_Settings.m_fMaxFOV)
        {
            s_fModeInitialFOV = (TheCamera.m_nActiveCam < 3)
                ? TheCamera.m_aCams[TheCamera.m_nActiveCam].m_fFOV
                : DEFAULT_FOV;
        }
        if (!std::isfinite(s_fModeInitialFOV) || s_fModeInitialFOV < s_Settings.m_fMinFOV || s_fModeInitialFOV > s_Settings.m_fMaxFOV)
            s_fModeInitialFOV = DEFAULT_FOV;
    }

    void ChangeGameHour(int direction)
    {
        const int hour = CalculateWrappedGameHour(
            static_cast<int>(CClock::ms_nGameClockHours), direction, s_Settings.m_nHoursPerPress);
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
        s_nSelectedWeatherPreset = weatherId;
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
            const bool pressed = WasDigitPressed(s_InputTracker, digit);
            if (pressed && pressedDigit < 0)
                pressedDigit = digit;
        }
        const bool zeroPressed = WasDigitPressed(s_InputTracker, 0);

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
        return "FOV " + std::to_string(static_cast<int>(std::round(s_fCameraFOV))) +
            " | time " + (hour < 10 ? "0" : "") + std::to_string(hour) + ":" +
            (minute < 10 ? "0" : "") + std::to_string(minute) +
            " | weather " + std::to_string(weather);
    }

    void DrawControlHelp()
    {
        if (!s_CameraModeController.IsActive() || !s_bControlHelpVisible || s_bSuppressControlHelpForCapture || s_InputTracker.IsGameMenuInputActive())
            return;

        const auto commonHelp = BuildCommonCameraHelpLines(s_Settings);

        // Keep the help compact and unobtrusive. FontSize in AmazingSnapshot.ini
        // scales both axes proportionally to the default subtitle font size.
        const float fontRatio = s_Settings.m_fHelpFontSize / 0.38f;
        const float fontScaleX = s_Settings.m_fHelpFontSize;
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
            GetKeyLabel(s_Settings.m_nKeyCapturePhoto) + ": save photo to Gallery",
            GetKeyLabel(s_Settings.m_nKeyFreezeEffects) + ": effects hold " +
                (HasPendingSceneEffectsPatches(s_CameraModeController.Snapshot()) ? "ON" : "OFF"),
            GetKeyLabel(s_Settings.m_nKeyWideScreen) + ": widescreen",
            "Time hour: " + GetKeyLabel(s_Settings.m_nKeyHourDown) + "/" +
                GetKeyLabel(s_Settings.m_nKeyHourUp) + " or Num-/Num+",
            "1-0: weather 0-9    Shift+1-9: weather 10-18",
            "Ctrl+1-4: weather 19-22",
            BuildHudHelpLine(s_CameraModeController.IsActive() && s_Settings.m_bHideHud),
            BuildCameraTimeHelpLine(s_bFreeCameraEnabled, s_bTimePlaybackEnabled, s_InputTracker.IsGameMenuInputActive()),
            GetKeyLabel(s_Settings.m_nKeyShowHelp) + ": hide/show help",
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
        s_strPhotoStatus = message;
        s_nPhotoStatusUntil = GetTickCount() + 3000;
    }

    void DrawPhotoStatus()
    {
        if (s_InputTracker.IsGameMenuInputActive() || s_strPhotoStatus.empty() ||
            static_cast<LONG>(GetTickCount() - s_nPhotoStatusUntil) >= 0)
            return;

        const float fontRatio = s_Settings.m_fHelpFontSize / 0.38f;
        const float fontScaleX = s_Settings.m_fHelpFontSize;
        const float fontScaleY = 0.80f * fontRatio;
        const float lineSpacing = 18.0f * fontRatio;
        const float y = s_bControlHelpVisible ? 24.0f + 17.0f * lineSpacing : 24.0f;
        CFont::SetFontStyle(FONT_SUBTITLES);
        CFont::SetColor(CRGBA(255, 255, 255, 255));
        CFont::SetDropShadowPosition(1);
        CFont::SetBackground(false, false);
        CFont::SetProportional(true);
        CFont::SetOrientation(ALIGN_LEFT);
        CFont::SetWrapx(620.0f);
        CFont::SetScale(fontScaleX, fontScaleY);
        CFont::PrintString(22.0f, y, s_strPhotoStatus.c_str());
    }

    // Invoke the game's own TAKE_PHOTO SCM command (opcode 0x0A1E).
    // It captures the current screen without HUD and lets GTA SA save the photo
    // to its native Gallery directory. The argument is the game's boolean flag.
    void TakePhotoUsingGameSystem()
    {
        // TAKE_PHOTO can internally restore GTA's default timer scale. Preserve
        // the scale that was active immediately before invoking the SCM opcode,
        // otherwise a frozen/slow-time screenshot session starts running again.
        const float timeScaleBeforePhoto = CTimer::ms_fTimeScale;
        static const std::array<unsigned char, 6> s_PhotoCommand = {
            0x1E, 0x0A, // opcode 0x0A1E: TAKE_PHOTO
            SCRIPTPARAM_STATIC_INT_8BITS, 1,
            0x00, 0x00 // harmless NOP terminator after the one command
        };

        CRunningScript script = {};
        script.m_pBaseIP = const_cast<unsigned char*>(s_PhotoCommand.data());
        script.m_pCurrentIP = const_cast<unsigned char*>(s_PhotoCommand.data());
        script.m_bIsActive = true;
        script.m_bCondResult = true;
        script.m_bIsMission = false;
        script.ProcessOneCommand();
        CTimer::ms_fTimeScale = timeScaleBeforePhoto;
    }

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

    void ExitScreenshotMode(ModeExitReason reason = ModeExitReason::UserToggle);

    void EnterScreenshotMode()
    {
        if (!CanEnterScreenshotMode())
            return;
        if (!s_CameraModeController.BeginEnter())
            return;

        SaveScreenshotModeState();
        CaptureCameraPose();
        s_fModeInitialFOV = s_fCameraFOV;
        s_bFreeCameraEnabled = true;
        s_bTimePlaybackEnabled = !s_Settings.m_bFreezeTime;
        s_nLastUpdateTick = GetTickCount();

        if (s_Settings.m_bHideHud)
            HideInterface();
        if (s_CameraModeController.NeedsRecovery())
        {
            ExitScreenshotMode(ModeExitReason::EntryFailure);
            return;
        }

        SetSceneEffectsFrozen(s_Settings.m_bFreezeEffectsOnStart);
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

        s_bControlHelpVisible = s_Settings.m_bShowHelpOnStart;
        ActivateFixedCamera();
        ReassertTimeFreeze();
        s_CameraModeController.MarkActive();
        if (s_CameraModeController.IsActive())
            AudioEngine.ReportFrontendAudioEvent(AE_FRONTEND_SELECT, 1.0f, 1.0f);
    }

    void ExitScreenshotMode(ModeExitReason reason)
    {
        if (!s_CameraModeController.BeginExit(reason))
            return;

        ModeSnapshot& snapshot = s_CameraModeController.Snapshot();
        const bool sessionTransition = snapshot.IsSessionTransition();
        bool restorationComplete = true;

        s_bSuppressControlHelpForCapture = false;
        s_bFreeCameraEnabled = false;
        s_bTimePlaybackEnabled = false;
        s_bControlHelpVisible = false;
        s_bPhotoCaptureRequested = false;

        if (snapshot.NeedsRestore(ModeResource::SceneEffectsPatch) || HasPendingSceneEffectsPatches(snapshot))
        {
            if (RestoreSceneEffectsPatches())
                snapshot.MarkRestored(ModeResource::SceneEffectsPatch);
            else
                restorationComplete = false;
        }
        if (snapshot.NeedsRestore(ModeResource::NativeHudPatch) || HasPendingNativeHudPatches(snapshot))
        {
            if (RestoreNativeHudDrawing())
                snapshot.MarkRestored(ModeResource::NativeHudPatch);
            else
                restorationComplete = false;
        }

        if (sessionTransition)
        {
            // The upcoming session owns its own runtime state; only code patches
            // and the input hook are restored to avoid leaking hooks across saves.
            snapshot.MarkRestored(ModeResource::CameraOwnership);
            snapshot.MarkRestored(ModeResource::CameraFOV);
            snapshot.MarkRestored(ModeResource::DrawFOV);
            snapshot.MarkRestored(ModeResource::WideScreen);
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
                for (int i = 0; i < 3; ++i)
                    TheCamera.m_aCams[i].m_fFOV = snapshot.m_afSavedCameraFOV[i];
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
                if (snapshot.m_bSavedWideScreen)
                    TheCamera.SetWideScreenOn();
                else
                    TheCamera.SetWideScreenOff();
                snapshot.MarkRestored(ModeResource::WideScreen);
            }
            if (snapshot.NeedsRestore(ModeResource::HudVisibility) && snapshot.IsCaptured(ModeResource::HudVisibility))
            {
                CHud::m_Wants_To_Draw_Hud = snapshot.m_bSavedHudVisible;
                snapshot.MarkRestored(ModeResource::HudVisibility);
            }
            if (snapshot.NeedsRestore(ModeResource::HudMarkers) && snapshot.IsCaptured(ModeResource::HudMarkers))
            {
                CHud::m_bDraw3dMarkers = snapshot.m_bSavedHudMarkersVisible;
                snapshot.MarkRestored(ModeResource::HudMarkers);
            }
            if (snapshot.NeedsRestore(ModeResource::GameSpecificHudFlags) && snapshot.IsCaptured(ModeResource::GameSpecificHudFlags))
            {
                CHud::bScriptDontDisplayRadar = snapshot.m_bSavedScriptRadarHidden;
                CHud::bScriptDontDisplayAreaName = snapshot.m_bSavedAreaNameHidden;
                CHud::bScriptDontDisplayVehicleName = snapshot.m_bSavedVehicleNameHidden;
                CHud::bDrawClock = snapshot.m_bSavedClockVisible;
                snapshot.MarkRestored(ModeResource::GameSpecificHudFlags);
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
            OutputDebugStringA("AmazingSnapshot SA: mode exit incomplete; restoration will be retried.\n");
    }

    void ToggleWideScreen()
    {
        const bool wasWideScreenOn = TheCamera.m_bWideScreenOn;
        s_bWideScreenCurrent = !s_bWideScreenCurrent;
        if (s_bWideScreenCurrent)
            TheCamera.SetWideScreenOn();
        else
            TheCamera.SetWideScreenOff();

        if (s_CameraModeController.IsActive() && wasWideScreenOn != s_bWideScreenCurrent)
            s_CameraModeController.Snapshot().MarkApplied(ModeResource::WideScreen);
    }

    void ApplyScreenshotFOV()
    {
        if (!s_CameraModeController.IsActive())
            return;

        s_fCameraFOV = ClampFloat(s_fCameraFOV, s_Settings.m_fMinFOV, s_Settings.m_fMaxFOV);
        // The active projection is CDraw's FOV, which native SA camera modes can
        // reset to 70 degrees. Apply projection first, then restore the active CCam
        // value as well; this prevents native camera processing from discarding a zoom step.
        const bool drawFovChanged = CDraw::ms_fFOV != s_fCameraFOV;
        const bool cameraFovChanged = TheCamera.m_nActiveCam < 3 &&
            TheCamera.m_aCams[TheCamera.m_nActiveCam].m_fFOV != s_fCameraFOV;
        CDraw::SetFOV(s_fCameraFOV);
        CDraw::ms_fFOV = s_fCameraFOV;
        if (TheCamera.m_nActiveCam < 3)
            TheCamera.m_aCams[TheCamera.m_nActiveCam].m_fFOV = s_fCameraFOV;
        if (cameraFovChanged)
            s_CameraModeController.Snapshot().MarkApplied(ModeResource::CameraFOV);
        if (drawFovChanged)
            s_CameraModeController.Snapshot().MarkApplied(ModeResource::DrawFOV);
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
        const float fovDeltaTime = ClampFloat(elapsedSeconds, 0.0f, 0.25f);

        ProcessEnvironmentHotkeys();
        if (WasKeyPressed(s_Settings.m_nKeyWideScreen))
            ToggleWideScreen();

        const bool zoomIn = IsCameraZoomInDown(s_InputTracker, s_Settings);
        const bool zoomOut = IsCameraZoomOutDown(s_InputTracker, s_Settings);
        const bool resetPressed = WasKeyPressed(s_Settings.m_nKeyResetZoom);
        s_fCameraFOV = UpdateContinuousCameraFOV(s_fCameraFOV, s_fModeInitialFOV,
            s_Settings, zoomIn, zoomOut, resetPressed, fovDeltaTime);

        if (!s_bFreeCameraEnabled)
            return;

        const bool lockYaw = IsMouseButtonDown(VK_LBUTTON);
        const bool lockPitch = IsMouseButtonDown(VK_RBUTTON);
        const CMouseControllerState& mouse = CPad::NewMouseControllerState;
        if (!lockYaw) s_fCameraYaw -= mouse.x * s_Settings.m_fMouseSensitivity;
        if (!lockPitch) s_fCameraPitch += mouse.y * s_Settings.m_fMouseSensitivity;
        s_fCameraPitch = ClampFloat(s_fCameraPitch, MIN_PITCH, MAX_PITCH);

        const Vector3 moveForward = ForwardFromYawPitch(s_fCameraYaw, s_fCameraPitch);
        const Vector3 moveRight = RightFromYaw(s_fCameraYaw);
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
        s_vecCameraPosition.x += (moveForward.x * forwardInput + moveRight.x * rightInput) * speed * deltaTime;
        s_vecCameraPosition.y += (moveForward.y * forwardInput + moveRight.y * rightInput) * speed * deltaTime;
        s_vecCameraPosition.z += (moveForward.z * forwardInput + verticalInput) * speed * deltaTime;
        s_vecCameraForward = CalculateForwardVector();
    }
    bool IsGameSessionTransitionPending()
    {
        // Native frontend flags are set when New Game/Restart or Load Game is
        // confirmed. Do not use m_bGameNotLoaded: Alt+V is intentionally allowed
        // without a startup-readiness gate.
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

        // Reacquire GTA's fixed-camera controller after Pause/Resume; preserve the
        // plugin's stored pose, FOV and selected time-playback state.
        s_InputTracker.Reset();
        s_nLastUpdateTick = GetTickCount();
        // Drop pause/frontend camera ownership before reacquiring the configured
        // screenshot camera. Keep the saved free-camera pose and FOV unchanged.
        TheCamera.RestoreWithJumpCut();
        if (s_bFreeCameraEnabled)
            ActivateFixedCamera();

        if (s_Settings.m_bHideHud)
            HideInterface();
        if (CPad* pad = CPad::GetPad(0))
        {
            if (pad->DisablePlayerControls != 1)
            {
                pad->DisablePlayerControls = 1;
                s_CameraModeController.Snapshot().MarkApplied(ModeResource::PlayerControls);
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

        // Keep HUD/player restrictions maintained on the game-process path, but
        // process user input exactly once from drawingEvent. Hook installation and
        // focus sampling are frame-side; no global hook remains during normal play.
        s_CameraModeController.SynchronizeInputCapture();
        if (!s_CameraModeController.IsActive())
            return;

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

        ReassertTimeFreeze();
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
        // Match the working III/VC input path: process mode toggles and movement
        // here, so keyboard controls continue to work while simulation time is frozen.
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

            const bool freezeEffectsPressed = WasKeyPressed(s_Settings.m_nKeyFreezeEffects);
            if (freezeEffectsPressed && !s_InputTracker.IsGameMenuInputActive())
                SetSceneEffectsFrozen(!HasPendingSceneEffectsPatches(s_CameraModeController.Snapshot()));
            if (s_CameraModeController.NeedsRecovery())
            {
                ExitScreenshotMode(ModeExitReason::RestoreFailure);
                return;
            }

            // Defer capture until the camera has been updated. GTA's native
            // TAKE_PHOTO opcode saves the image to the game's Gallery.
            const bool photoKeyPressed = WasKeyPressed(s_Settings.m_nKeyCapturePhoto);
            if (photoKeyPressed && !s_InputTracker.IsGameMenuInputActive())
                s_bPhotoCaptureRequested = true;

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
            }
            else
            {
                // Keep the native pause frontend in control while the scene is paused.
                s_nLastUpdateTick = GetTickCount();
            }

        }

        // Process the native photo command after applying the camera and before
        // drawing hints, so the help overlay is not included in the screenshot.
        if (s_bPhotoCaptureRequested && !s_InputTracker.IsGameMenuInputActive())
        {
            s_bPhotoCaptureRequested = false;
            // Hide the overlay for the complete native TAKE_PHOTO call as a guard
            // against any reentrant draw callback while the game captures the frame.
            s_bSuppressControlHelpForCapture = true;
            TakePhotoUsingGameSystem();
            s_bSuppressControlHelpForCapture = false;
            ShowPhotoStatus("Photo saved: GTA Gallery");
        }

        // Keep the freeze authoritative for the whole screenshot session rather
        // than applying it only on the transition into the frozen state. Native
        // photo capture and game scripts can both reset ms_fTimeScale to 1.0f.
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
