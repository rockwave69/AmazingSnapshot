#include "AmazingSnapshotCore.h"
#include "plugin.h"
#include "RenderWare.h"

#include <ShlObj.h>
#include <gdiplus.h>
#include <vector>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Gdiplus.lib")

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace AmazingSnapshot
{
    namespace
    {
        // Bounded copy without MSVC's deprecated strncpy warning.
        // The destination is always null-terminated when capacity is non-zero.
        void CopyCString(char* destination, std::size_t capacity, const char* source)
        {
            if (!destination || capacity == 0)
                return;

            if (!source)
            {
                destination[0] = '\0';
                return;
            }

            std::size_t length = std::strlen(source);
            if (length >= capacity)
                length = capacity - 1;
            if (length > 0)
                std::memcpy(destination, source, length);
            destination[length] = '\0';
        }

        float ReadIniFloat(const char* section, const char* key, float defaultValue,
                           const char* iniPath)
        {
            char defaultText[48] = {};
            char valueText[64] = {};
            std::snprintf(defaultText, sizeof(defaultText), "%.6f", defaultValue);
            GetPrivateProfileStringA(section, key, defaultText, valueText,
                static_cast<DWORD>(sizeof(valueText)), iniPath);

            char* end = nullptr;
            const float value = std::strtof(valueText, &end);
            if (end == valueText || !std::isfinite(value))
                return defaultValue;
            while (*end && std::isspace(static_cast<unsigned char>(*end)))
                ++end;
            return *end == '\0' ? value : defaultValue;
        }

        bool ReadIniBool(const char* section, const char* key, bool defaultValue,
                         const char* iniPath)
        {
            char defaultText[8] = {};
            char valueText[32] = {};
            std::snprintf(defaultText, sizeof(defaultText), "%d", defaultValue ? 1 : 0);
            GetPrivateProfileStringA(section, key, defaultText, valueText,
                static_cast<DWORD>(sizeof(valueText)), iniPath);

            std::string value(valueText);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
                value.erase(value.begin());
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
                value.pop_back();
            for (char& character : value)
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));

            if (value == "1" || value == "true" || value == "yes" || value == "on")
                return true;
            if (value == "0" || value == "false" || value == "no" || value == "off")
                return false;
            return defaultValue;
        }

        constexpr std::size_t PHYSICAL_KEY_COUNT = KeyCode::ScanCodeCount;
        std::array<std::atomic_bool, PHYSICAL_KEY_COUNT> s_physicalKeyDown{};
        std::atomic_bool s_keyboardCaptureActive{ false };
        std::atomic<int> s_altChordKeyScanCode{ -1 };
        std::atomic_bool s_altChordSuppressionActive{ false };
        std::atomic_bool s_gameMenuInputActive{ false };
        std::atomic_bool s_nativeGameMenuObservedActive{ false };
        std::atomic_bool s_escapeKeyUpWasForwarded{ false };
        HHOOK s_keyboardHook = nullptr;
        DWORD s_nextKeyboardHookAttemptTick = 0;
        std::atomic_bool s_inputProcessForeground{ false };
        std::atomic<HWND> s_expectedForegroundWindow{ nullptr };

        bool IsCurrentProcessForeground()
        {
            const HWND foregroundWindow = GetForegroundWindow();
            if (!foregroundWindow)
            {
                s_expectedForegroundWindow.store(nullptr, std::memory_order_relaxed);
                return false;
            }

            DWORD foregroundProcessId = 0;
            GetWindowThreadProcessId(foregroundWindow, &foregroundProcessId);
            const bool isGameForeground = foregroundProcessId == GetCurrentProcessId();
            s_expectedForegroundWindow.store(isGameForeground ? foregroundWindow : nullptr,
                std::memory_order_relaxed);
            return isGameForeground;
        }

        void ClearPhysicalKeyStates()
        {
            for (auto& state : s_physicalKeyDown)
                state.store(false, std::memory_order_relaxed);
        }

        int GetHookScanCode(const KBDLLHOOKSTRUCT& keyEvent)
        {
            const int scanCode = static_cast<int>(keyEvent.scanCode & 0xFFu);
            if (scanCode == 0)
                return -1;
            return scanCode | ((keyEvent.flags & LLKHF_EXTENDED) ? 0x100 : 0);
        }

        bool IsDownNow(int virtualKey)
        {
            return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
        }

        bool IsAltPhysicallyDownFromHookState()
        {
            return s_physicalKeyDown[KeyCode::LeftAlt].load(std::memory_order_relaxed) ||
                   s_physicalKeyDown[KeyCode::RightAlt].load(std::memory_order_relaxed);
        }

        bool IsControlPhysicallyDownFromHookState()
        {
            return s_physicalKeyDown[KeyCode::LeftControl].load(std::memory_order_relaxed) ||
                   s_physicalKeyDown[KeyCode::RightControl].load(std::memory_order_relaxed);
        }

        bool IsAnyAltDownNow()
        {
            return IsDownNow(VK_MENU) || IsDownNow(VK_LMENU) || IsDownNow(VK_RMENU);
        }

        // Keep normal Windows escape routes available even while the game's own
        // keyboard input is captured. Use the hook-maintained modifier state here;
        // querying multiple Windows key states on every global keyboard event was
        // unnecessary work and contributed to input latency.
        bool IsSystemShortcut(const KBDLLHOOKSTRUCT& keyEvent)
        {
            const bool altDown = IsAltPhysicallyDownFromHookState();
            const bool controlDown = IsControlPhysicallyDownFromHookState();

            switch (keyEvent.vkCode)
            {
            // Modifier keys alone do not invoke a GTA action, and must remain
            // visible to Windows so system combinations can be recognized.
            case VK_SHIFT:
            case VK_LSHIFT:
            case VK_RSHIFT:
            case VK_CONTROL:
            case VK_LCONTROL:
            case VK_RCONTROL:
            case VK_MENU:
            case VK_LMENU:
            case VK_RMENU:
            case VK_LWIN:
            case VK_RWIN:
                return true;
            case VK_TAB:     // Alt+Tab: switch to another application.
            case VK_F4:      // Alt+F4: close the game/window normally.
            case VK_SPACE:   // Alt+Space: native window menu.
                return altDown;
            case VK_ESCAPE:  // Ctrl+Esc / Ctrl+Shift+Esc: Windows shell / Task Manager.
                return controlDown;
            case VK_DELETE:  // Keep Ctrl+Alt+Delete available to Windows.
                return controlDown && altDown;
            default:
                return false;
            }
        }

        bool IsMenuNavigationKey(DWORD virtualKey)
        {
            switch (virtualKey)
            {
            case VK_ESCAPE:
            case VK_RETURN:
            case VK_BACK:
            case VK_UP:
            case VK_DOWN:
            case VK_LEFT:
            case VK_RIGHT:
            case VK_SPACE:
                return true;
            default:
                return false;
            }
        }

        LRESULT CALLBACK KeyboardHookProc(int code, WPARAM message, LPARAM data)
        {
            if (code != HC_ACTION || data == 0)
                return CallNextHookEx(s_keyboardHook, code, message, data);

            // The hook is installed only for Alt+mode-toggle detection or while the
            // screenshot mode captures input. Confirm the foreground HWND cheaply on
            // every hook event so that a minimized/background GTA cannot swallow keys
            // intended for another application. Avoid GetWindowThreadProcessId here:
            // it is only needed when sampling focus from the render callback.
            const HWND expectedWindow = s_expectedForegroundWindow.load(std::memory_order_relaxed);
            if (!s_inputProcessForeground.load(std::memory_order_relaxed) || !expectedWindow)
                return CallNextHookEx(s_keyboardHook, code, message, data);
            if (GetForegroundWindow() != expectedWindow)
            {
                // Focus may change while a minimized game stops drawing. Do not
                // swallow the other app's input, and clear held keys so they cannot
                // stick when GTA regains focus before the next render callback.
                s_inputProcessForeground.store(false, std::memory_order_relaxed);
                s_expectedForegroundWindow.store(nullptr, std::memory_order_relaxed);
                ClearPhysicalKeyStates();
                s_altChordSuppressionActive.store(false, std::memory_order_relaxed);
                return CallNextHookEx(s_keyboardHook, code, message, data);
            }

            if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN &&
                message != WM_KEYUP && message != WM_SYSKEYUP)
                return CallNextHookEx(s_keyboardHook, code, message, data);

            const KBDLLHOOKSTRUCT* keyEvent = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
            const int scanCode = GetHookScanCode(*keyEvent);
            const bool isDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            bool wasDown = false;
            if (scanCode >= 0 && scanCode < static_cast<int>(s_physicalKeyDown.size()))
            {
                wasDown = s_physicalKeyDown[static_cast<std::size_t>(scanCode)].load(std::memory_order_relaxed);
                s_physicalKeyDown[static_cast<std::size_t>(scanCode)].store(
                    isDown, std::memory_order_relaxed);
            }

            // Alt+ModeToggle belongs to this plugin. Consume both edges so GTA
            // never receives the V that would otherwise switch the native camera.
            const int chordKey = s_altChordKeyScanCode.load(std::memory_order_relaxed);
            if (chordKey >= 0 && scanCode == chordKey)
            {
                if (isDown && IsAltPhysicallyDownFromHookState())
                {
                    s_altChordSuppressionActive.store(true, std::memory_order_relaxed);
                    return 1;
                }
                if (!isDown && s_altChordSuppressionActive.exchange(false, std::memory_order_relaxed))
                    return 1;
            }

            if (s_keyboardCaptureActive.load(std::memory_order_relaxed))
            {
                const bool controlDown = IsControlPhysicallyDownFromHookState();
                const bool altDown = IsAltPhysicallyDownFromHookState();
                const bool plainEscape = keyEvent->vkCode == VK_ESCAPE && !controlDown && !altDown;

                // Esc belongs to the game's pause menu, never to plugin exit.
                // Keep menu input pass-through enabled until the native menu
                // manager reports that the menu has actually closed.
                if (plainEscape)
                {
                    if (isDown && !wasDown)
                    {
                        s_gameMenuInputActive.store(true, std::memory_order_relaxed);
                        s_escapeKeyUpWasForwarded.store(true, std::memory_order_relaxed);
                    }
                    if (isDown || s_escapeKeyUpWasForwarded.exchange(false, std::memory_order_relaxed))
                        return CallNextHookEx(s_keyboardHook, code, message, data);
                }

                // Once Esc opens the menu, allow its navigation keys, but keep
                // replay/function/gameplay hotkeys blocked to avoid collisions.
                if (s_gameMenuInputActive.load(std::memory_order_relaxed) &&
                    IsMenuNavigationKey(keyEvent->vkCode))
                {
                    return CallNextHookEx(s_keyboardHook, code, message, data);
                }

                // Preserve Windows shortcuts; all other game keyboard input is captured.
                if (!IsSystemShortcut(*keyEvent))
                    return 1;
            }
            return CallNextHookEx(s_keyboardHook, code, message, data);
        }

        bool EnsureKeyboardHookInstalled()
        {
            const HWND expectedWindow = s_expectedForegroundWindow.load(std::memory_order_relaxed);
            if (!s_inputProcessForeground.load(std::memory_order_relaxed) || !expectedWindow ||
                GetForegroundWindow() != expectedWindow)
                return false;

            if (s_keyboardHook)
                return true;

            const DWORD now = GetTickCount();
            if (s_nextKeyboardHookAttemptTick != 0 && now < s_nextKeyboardHookAttemptTick)
                return false;
            s_nextKeyboardHookAttemptTick = now + 1000;

            HMODULE module = nullptr;
            const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
            if (!GetModuleHandleExA(flags,
                    reinterpret_cast<LPCSTR>(&KeyboardHookProc), &module))
            {
                OutputDebugStringA("AmazingSnapshot: could not resolve keyboard hook module; retrying.\n");
                return false;
            }

            s_keyboardHook = SetWindowsHookExA(WH_KEYBOARD_LL, KeyboardHookProc, module, 0);
            if (!s_keyboardHook)
            {
                OutputDebugStringA("AmazingSnapshot: physical keyboard hook could not be installed; retrying.\n");
                return false;
            }
            ClearPhysicalKeyStates();
            // Alt was usually pressed in the frame that caused this lazy install,
            // before the hook could observe its WM_SYSKEYDOWN edge. Seed modifier
            // state once so Alt+V and Ctrl/Shift menu chords still work immediately.
            s_physicalKeyDown[KeyCode::LeftAlt].store(IsDownNow(VK_LMENU), std::memory_order_relaxed);
            s_physicalKeyDown[KeyCode::RightAlt].store(IsDownNow(VK_RMENU), std::memory_order_relaxed);
            if (!IsAltPhysicallyDownFromHookState() && IsDownNow(VK_MENU))
                s_physicalKeyDown[KeyCode::LeftAlt].store(true, std::memory_order_relaxed);
            s_physicalKeyDown[KeyCode::LeftControl].store(IsDownNow(VK_LCONTROL), std::memory_order_relaxed);
            s_physicalKeyDown[KeyCode::RightControl].store(IsDownNow(VK_RCONTROL), std::memory_order_relaxed);
            s_physicalKeyDown[KeyCode::LeftShift].store(IsDownNow(VK_LSHIFT), std::memory_order_relaxed);
            s_physicalKeyDown[KeyCode::RightShift].store(IsDownNow(VK_RSHIFT), std::memory_order_relaxed);
            return true;
        }

        int LetterScanCode(char letter)
        {
            // Physical Set-1 locations for the conventional PC keyboard layout.
            // These values identify positions, not the glyph emitted by the current layout.
            switch (letter)
            {
            case 'A': return KeyCode::A; case 'B': return KeyCode::B;
            case 'C': return KeyCode::C; case 'D': return KeyCode::D;
            case 'E': return KeyCode::E; case 'F': return KeyCode::F;
            case 'G': return KeyCode::G; case 'H': return KeyCode::H;
            case 'I': return KeyCode::I; case 'J': return KeyCode::J;
            case 'K': return KeyCode::K; case 'L': return KeyCode::L;
            case 'M': return KeyCode::M; case 'N': return KeyCode::N;
            case 'O': return KeyCode::O; case 'P': return KeyCode::P;
            case 'Q': return KeyCode::Q; case 'R': return KeyCode::R;
            case 'S': return KeyCode::S; case 'T': return KeyCode::T;
            case 'U': return KeyCode::U; case 'V': return KeyCode::V;
            case 'W': return KeyCode::W; case 'X': return KeyCode::X;
            case 'Y': return KeyCode::Y; case 'Z': return KeyCode::Z;
            default: return -1;
            }
        }

        int DigitScanCode(char digit)
        {
            if (digit >= '1' && digit <= '9')
                return KeyCode::NumberRowKey(digit - '0');
            if (digit == '0')
                return KeyCode::Digit0;
            return -1;
        }

        int ParseScanCode(const char* text, int fallback)
        {
            if (!text)
                return fallback;

            std::string key(text);
            while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front())))
                key.erase(key.begin());
            while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back())))
                key.pop_back();
            for (char& character : key)
                character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
            if (key.empty())
                return fallback;

            // New format: SC_0xNN or SC_decimal. Bare numbers and 0xNN are also accepted.
            std::string numeric = key;
            if (numeric.compare(0, 3, "SC_") == 0)
                numeric.erase(0, 3);
            const bool looksNumeric = !numeric.empty() &&
                (std::isdigit(static_cast<unsigned char>(numeric[0])) ||
                 (numeric.size() > 2 && numeric[0] == '0' && numeric[1] == 'X'));
            if (looksNumeric)
            {
                char* end = nullptr;
                const long value = std::strtol(numeric.c_str(), &end, 0);
                if (end != numeric.c_str() && *end == '\0' && value >= 0 &&
                    value < static_cast<long>(KeyCode::StateCount))
                    return static_cast<int>(value);
                return fallback;
            }

            if (key.size() == 1)
            {
                const int letterCode = LetterScanCode(key[0]);
                if (letterCode >= 0)
                    return letterCode;
                const int digitCode = DigitScanCode(key[0]);
                if (digitCode >= 0)
                    return digitCode;
                if (key[0] == '[') return KeyCode::LeftBracket;
                if (key[0] == ']') return KeyCode::RightBracket;
                if (key[0] == '-' || key[0] == '_') return KeyCode::Minus;
                if (key[0] == '=' || key[0] == '+') return KeyCode::Equals;
                if (key[0] == ',') return KeyCode::Comma;
                if (key[0] == '.') return KeyCode::Period;
            }

            if (key.size() >= 2 && key[0] == 'F')
            {
                char* end = nullptr;
                const long number = std::strtol(key.c_str() + 1, &end, 10);
                if (end != key.c_str() + 1 && *end == '\0')
                {
                    if (number >= 1 && number <= 10)
                        return KeyCode::F1 + static_cast<int>(number) - 1;
                    if (number == 11) return KeyCode::F11;
                    if (number == 12) return KeyCode::F12;
                }
            }

            if (key == "SPACE") return KeyCode::Space;
            if (key == "TAB") return KeyCode::Tab;
            if (key == "ESC" || key == "ESCAPE") return KeyCode::Escape;
            if (key == "SHIFT" || key == "ANY_SHIFT") return KeyCode::Shift;
            if (key == "LEFTSHIFT" || key == "LSHIFT") return KeyCode::LeftShift;
            if (key == "RIGHTSHIFT" || key == "RSHIFT") return KeyCode::RightShift;
            if (key == "CTRL" || key == "CONTROL" || key == "ANY_CTRL") return KeyCode::Control;
            if (key == "LEFTCTRL" || key == "LCTRL") return KeyCode::LeftControl;
            if (key == "RIGHTCTRL" || key == "RCTRL") return KeyCode::RightControl;
            if (key == "ALT" || key == "ANY_ALT") return KeyCode::Alt;
            if (key == "LEFTALT" || key == "LALT") return KeyCode::LeftAlt;
            if (key == "RIGHTALT" || key == "RALT") return KeyCode::RightAlt;
            if (key == "PAGEUP" || key == "PGUP") return KeyCode::PageUp;
            if (key == "PAGEDOWN" || key == "PGDN") return KeyCode::PageDown;
            if (key == "UP") return KeyCode::Up;
            if (key == "DOWN") return KeyCode::Down;
            if (key == "LEFT") return KeyCode::Left;
            if (key == "RIGHT") return KeyCode::Right;
            if (key == "HOME") return KeyCode::Home;
            if (key == "END") return KeyCode::End;
            if (key == "INSERT" || key == "INS") return KeyCode::Insert;
            if (key == "DELETE" || key == "DEL") return KeyCode::Delete;
            if (key == "LBRACKET" || key == "OEM_4") return KeyCode::LeftBracket;
            if (key == "RBRACKET" || key == "OEM_6") return KeyCode::RightBracket;
            if (key == "COMMA" || key == "OEM_COMMA") return KeyCode::Comma;
            if (key == "PERIOD" || key == "OEM_PERIOD") return KeyCode::Period;
            if (key == "OEM_102" || key == "ISO_LESS_GREATER" || key == "<>" ) return KeyCode::IsoLessGreater;
            if (key == "OEM_MINUS" || key == "MINUS") return KeyCode::Minus;
            if (key == "OEM_PLUS" || key == "PLUS" || key == "EQUALS") return KeyCode::Equals;
            if (key == "NUM_PLUS" || key == "ADD" || key == "NUMPADPLUS") return KeyCode::NumPlus;
            if (key == "NUM_MINUS" || key == "SUBTRACT" || key == "NUMPADMINUS") return KeyCode::NumMinus;
            if (key == "NUMPAD0") return KeyCode::Num0;
            if (key == "NUMPAD1") return KeyCode::Num1;
            if (key == "NUMPAD2") return KeyCode::Num2;
            if (key == "NUMPAD3") return KeyCode::Num3;
            if (key == "NUMPAD4") return KeyCode::Num4;
            if (key == "NUMPAD5") return KeyCode::Num5;
            if (key == "NUMPAD6") return KeyCode::Num6;
            if (key == "NUMPAD7") return KeyCode::Num7;
            if (key == "NUMPAD8") return KeyCode::Num8;
            if (key == "NUMPAD9") return KeyCode::Num9;
            if (key == "ENTER" || key == "RETURN") return KeyCode::Enter;
            if (key == "NUMENTER") return KeyCode::NumEnter;
            if (key == "BACKSPACE") return KeyCode::Backspace;
            return fallback;
        }

        int ReadIniKey(const char* name, const char* defaultValue, int fallback,
                       const char* iniPath)
        {
            char value[64] = {};
            GetPrivateProfileStringA("Keys", name, defaultValue, value,
                static_cast<DWORD>(sizeof(value)), iniPath);
            return ParseScanCode(value, fallback);
        }

        bool IsPhysicalKeyDown(int scanCode)
        {
            if (scanCode < 0 || scanCode >= static_cast<int>(PHYSICAL_KEY_COUNT) ||
                !s_inputProcessForeground.load(std::memory_order_relaxed))
                return false;

            // While capture is active, the low-level hook deliberately swallows
            // gameplay key events. GetAsyncKeyState is not reliable for every
            // swallowed event, so use the physical state captured by the hook as
            // the source of truth. This is essential for camera movement and Esc
            // to keep working while other GTA hotkeys are blocked.
            if ((s_keyboardCaptureActive.load(std::memory_order_relaxed) ||
                 s_altChordSuppressionActive.load(std::memory_order_relaxed)) && s_keyboardHook)
            {
                // The hook also swallows the configured key for Alt+mode-toggle
                // before capture mode starts, so use the hook state for that chord
                // too; GetAsyncKeyState may not report a swallowed key reliably.
                return s_physicalKeyDown[static_cast<std::size_t>(scanCode)].load(std::memory_order_relaxed);
            }

            // Outside capture mode, polling the current keyboard layout is a useful
            // fallback for game window/message-loop variants that miss hook edges.
            UINT windowsScanCode = static_cast<UINT>(scanCode & 0xFF);
            if ((scanCode & 0x100) != 0)
                windowsScanCode |= 0xE000;
            const HKL layout = GetKeyboardLayout(0);
            const UINT virtualKey = MapVirtualKeyExA(windowsScanCode, MAPVK_VSC_TO_VK_EX, layout);
            if (virtualKey != 0)
                return (GetAsyncKeyState(static_cast<int>(virtualKey)) & 0x8000) != 0;

            if (!s_keyboardHook)
                return false;
            return s_physicalKeyDown[static_cast<std::size_t>(scanCode)].load(std::memory_order_relaxed);
        }

        bool EnsureDirectoryExists(const std::string& directory)
        {
            if (CreateDirectoryA(directory.c_str(), nullptr))
                return true;
            if (GetLastError() != ERROR_ALREADY_EXISTS)
                return false;
            const DWORD attributes = GetFileAttributesA(directory.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }

        bool TryGetPortableGalleryDirectory(std::string& galleryPath)
        {
            char executablePath[MAX_PATH] = {};
            const DWORD pathLength = GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
            if (pathLength == 0 || pathLength >= MAX_PATH)
                return false;

            char* lastSeparator = std::strrchr(executablePath, '\\');
            if (!lastSeparator)
                lastSeparator = std::strrchr(executablePath, '/');
            if (!lastSeparator)
                return false;
            *lastSeparator = '\0';

            const std::string userFilesPath = std::string(executablePath) + "\\userfiles";
            if (!EnsureDirectoryExists(userFilesPath))
                return false;

            const std::string candidateGalleryPath = userFilesPath + "\\Gallery";
            if (!EnsureDirectoryExists(candidateGalleryPath))
                return false;

            // Follow portablegta.cpp: userfiles lives next to the game executable,
            // and Gallery is a child folder shared by all three game adapters.
            galleryPath = candidateGalleryPath;
            return true;
        }

        bool TryGetDocumentsGalleryDirectory(const char* gameUserFilesDirectory, std::string& galleryPath)
        {
            if (!gameUserFilesDirectory || gameUserFilesDirectory[0] == '\0')
                return false;

            char documentsPath[MAX_PATH] = {};
            if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL | CSIDL_FLAG_CREATE,
                nullptr, SHGFP_TYPE_CURRENT, documentsPath)))
            {
                return false;
            }

            const std::string userFilesPath = std::string(documentsPath) + "\\" + gameUserFilesDirectory;
            if (!EnsureDirectoryExists(userFilesPath))
                return false;

            const std::string candidateGalleryPath = userFilesPath + "\\Gallery";
            if (!EnsureDirectoryExists(candidateGalleryPath))
                return false;

            galleryPath = candidateGalleryPath;
            return true;
        }

        bool GetNextGalleryFilePath(const char* gameUserFilesDirectory, std::string& outputPath)
        {
            std::string galleryPath;
            bool usingPortablePath = false;
            if (!GetGalleryDirectory(gameUserFilesDirectory, galleryPath, usingPortablePath))
                return false;
            (void)usingPortablePath; // The resolved directory is all the JPEG saver needs.

            // Match the stock San Andreas gallery naming convention and never overwrite.
            for (unsigned int index = 1; index <= 99999; ++index)
            {
                char fileName[64] = {};
                std::snprintf(fileName, sizeof(fileName), "gallery%u.jpg", index);
                const std::string candidate = galleryPath + "\\" + fileName;
                const DWORD attributes = GetFileAttributesA(candidate.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND)
                {
                    outputPath = candidate;
                    return true;
                }
            }
            return false;
        }

        bool EncodeBgrPixelsAsJpeg(const std::string& path, UINT width, UINT height,
            UINT stride, const std::vector<unsigned char>& pixels)
        {
            const std::uint64_t minimumRowBytes = static_cast<std::uint64_t>(width) * 3u;
            const std::uint64_t requiredBufferSize = static_cast<std::uint64_t>(stride) * height;
            if (width == 0 || height == 0 ||
                width > static_cast<UINT>(std::numeric_limits<INT>::max()) ||
                height > static_cast<UINT>(std::numeric_limits<INT>::max()) ||
                stride < minimumRowBytes || (stride & 3u) != 0 ||
                stride > static_cast<UINT>(std::numeric_limits<INT>::max()) ||
                requiredBufferSize > pixels.size() || pixels.empty())
            {
                return false;
            }

            const int wideLength = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
            if (wideLength <= 0)
                return false;
            std::vector<wchar_t> widePath(static_cast<std::size_t>(wideLength), L'\0');
            if (MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, widePath.data(), wideLength) <= 0)
                return false;

            // Use the Windows GDI+ JPEG encoder instead of WIC. Some Plugin-SDK
            // setups include the legacy DirectX SDK before the Windows 10 SDK;
            // wincodec.h then collides with old DXGI definitions and does not see
            // the newer DXGI_JPEG_* types. GDI+ avoids that header dependency.
            Gdiplus::GdiplusStartupInput startupInput;
            ULONG_PTR gdiplusToken = 0;
            if (Gdiplus::GdiplusStartup(&gdiplusToken, &startupInput, nullptr) != Gdiplus::Ok)
                return false;

            bool saved = false;
            {
                // GDI+ requires a DWORD-aligned stride. CaptureRenderWareFrameToGallery
                // prepares the BGR rows with padding when the game resolution needs it.
                Gdiplus::Bitmap bitmap(static_cast<INT>(width), static_cast<INT>(height),
                    static_cast<INT>(stride), PixelFormat24bppRGB,
                    const_cast<BYTE*>(pixels.data()));

                if (bitmap.GetLastStatus() == Gdiplus::Ok)
                {
                    // Standard GDI+ JPEG encoder CLSID and quality parameter GUID.
                    static const CLSID jpegEncoderClsid =
                    { 0x557cf401, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
                    static const GUID jpegQualityGuid =
                    { 0x1d5be4b5, 0xfa4a, 0x452d, { 0x9c, 0xdd, 0x5d, 0xb3, 0x51, 0x05, 0xe7, 0xeb } };

                    ULONG quality = 92;
                    Gdiplus::EncoderParameters encoderParameters{};
                    encoderParameters.Count = 1;
                    encoderParameters.Parameter[0].Guid = jpegQualityGuid;
                    encoderParameters.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
                    encoderParameters.Parameter[0].NumberOfValues = 1;
                    encoderParameters.Parameter[0].Value = &quality;

                    saved = bitmap.Save(widePath.data(), &jpegEncoderClsid,
                        &encoderParameters) == Gdiplus::Ok;
                }
            }

            Gdiplus::GdiplusShutdown(gdiplusToken);
            return saved;
        }

        bool CaptureRenderWareFrameToGallery(void* renderWareCamera, const char* gameUserFilesDirectory,
            std::string& savedPath)
        {
            RwCamera* camera = static_cast<RwCamera*>(renderWareCamera);
            if (!camera || !camera->frameBuffer)
                return false;

            RwRaster* frameRaster = camera->frameBuffer;
            const RwInt32 rasterWidth = RwRasterGetWidth(frameRaster);
            const RwInt32 rasterHeight = RwRasterGetHeight(frameRaster);
            if (rasterWidth <= 0 || rasterHeight <= 0)
                return false;

            RwImage* image = RwImageCreate(rasterWidth, rasterHeight, 32);
            if (!image)
                return false;
            if (!RwImageAllocatePixels(image) || !RwImageSetFromRaster(image, frameRaster))
            {
                RwImageDestroy(image);
                return false;
            }
            if (!image->cpPixels || image->width <= 0 || image->height <= 0 ||
                image->depth != 32 || image->stride < image->width * 4)
            {
                RwImageDestroy(image);
                return false;
            }

            const std::uint32_t width = static_cast<std::uint32_t>(image->width);
            const std::uint32_t height = static_cast<std::uint32_t>(image->height);
            const std::uint64_t rawStride64 = static_cast<std::uint64_t>(width) * 3u;
            const std::uint64_t stride64 = (rawStride64 + 3u) & ~std::uint64_t(3u);
            const std::uint64_t bufferSize64 = stride64 * height;
            if (stride64 > MAXDWORD || stride64 > static_cast<std::uint64_t>(std::numeric_limits<INT>::max()) ||
                bufferSize64 > MAXDWORD)
            {
                RwImageDestroy(image);
                return false;
            }

            std::vector<unsigned char> bgrPixels(static_cast<std::size_t>(bufferSize64));
            for (std::uint32_t y = 0; y < height; ++y)
            {
                const unsigned char* sourceRow = image->cpPixels +
                    static_cast<std::size_t>(y) * static_cast<std::size_t>(image->stride);
                unsigned char* destinationRow = bgrPixels.data() +
                    static_cast<std::size_t>(y) * static_cast<std::size_t>(stride64);
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    const unsigned char* sourcePixel = sourceRow + static_cast<std::size_t>(x) * 4u;
                    unsigned char* destinationPixel = destinationRow + static_cast<std::size_t>(x) * 3u;
                    destinationPixel[0] = sourcePixel[2];
                    destinationPixel[1] = sourcePixel[1];
                    destinationPixel[2] = sourcePixel[0];
                }
            }

            std::string path;
            if (!GetNextGalleryFilePath(gameUserFilesDirectory, path))
            {
                RwImageDestroy(image);
                return false;
            }
            const bool saved = EncodeBgrPixelsAsJpeg(path, static_cast<UINT>(width),
                static_cast<UINT>(height), static_cast<UINT>(stride64), bgrPixels);
            RwImageDestroy(image);
            if (!saved)
            {
                DeleteFileA(path.c_str());
                return false;
            }
            savedPath = path;
            return true;
        }

    }

    bool GetGalleryDirectory(const char* gameUserFilesDirectory, std::string& outputPath,
        bool& usingPortablePath)
    {
        outputPath.clear();
        usingPortablePath = false;
        if (TryGetPortableGalleryDirectory(outputPath))
        {
            usingPortablePath = true;
            return true;
        }

        return TryGetDocumentsGalleryDirectory(gameUserFilesDirectory, outputPath);
    }

    bool SaveRenderWareFrameToGalleryJpeg(void* renderWareCamera, const char* gameUserFilesDirectory,
        std::string& savedPath)
    {
        return CaptureRenderWareFrameToGallery(renderWareCamera, gameUserFilesDirectory, savedPath);
    }

    Settings MakeDefaultSettings()
    {
        Settings settings{};
        settings.m_freezeTime = true;
        settings.m_hideHud = true;
        settings.m_freezeEffectsOnStart = true;
        settings.m_showHelpOnStart = true;
        settings.m_minFov = 10.0f;
        settings.m_maxFov = 150.0f;
        settings.m_zoomStep = 0.5f;
        settings.m_mouseSensitivity = 0.0026f;
        settings.m_baseMoveSpeed = 7.0f;
        settings.m_fastMoveMultiplier = 3.0f;
        settings.m_slowMoveMultiplier = 0.22f;
        settings.m_keyModeToggle = KeyCode::V;
        settings.m_keyToggleTimePlayback = KeyCode::F3;
        settings.m_keyWideScreen = KeyCode::Tab;
        settings.m_keyZoomIn = KeyCode::LeftBracket;
        settings.m_keyZoomOut = KeyCode::RightBracket;
        settings.m_keyResetZoom = KeyCode::P;
        settings.m_keyShowHelp = KeyCode::F1;
        settings.m_keyFreezeEffects = KeyCode::F2;
        settings.m_keyCapturePhoto = KeyCode::F12;
        settings.m_keyAscend = KeyCode::Space;
        settings.m_keyDescend = KeyCode::C;
        settings.m_keyForward = KeyCode::W;
        settings.m_keyBackward = KeyCode::S;
        settings.m_keyLeft = KeyCode::A;
        settings.m_keyRight = KeyCode::D;
        settings.m_keyMoveFast = KeyCode::Shift;
        settings.m_keyMoveSlow = KeyCode::Control;
        settings.m_helpFontSize = 0.45f;
        settings.m_hoursPerPress = 1;
        settings.m_keyHourDown = KeyCode::Minus;
        settings.m_keyHourUp = KeyCode::Equals;
        return settings;
    }

    const char* GetConfigurationPath(const char* iniFileName)
    {
        static char path[MAX_PATH] = {};
        static char cachedName[MAX_PATH] = {};
        const char* requestedName = (iniFileName && *iniFileName) ? iniFileName : "AmazingSnapshot.ini";
        if (path[0] != '\0' && std::strcmp(cachedName, requestedName) == 0)
            return path;

        path[0] = '\0';
        CopyCString(cachedName, sizeof(cachedName), requestedName);

        HMODULE pluginModule = nullptr;
        const DWORD moduleFlags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
        if (!GetModuleHandleExA(moduleFlags,
            reinterpret_cast<LPCSTR>(&GetConfigurationPath), &pluginModule))
        {
            CopyCString(path, sizeof(path), requestedName);
            return path;
        }

        char modulePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(pluginModule, modulePath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            CopyCString(path, sizeof(path), requestedName);
            return path;
        }

        char* lastBackslash = std::strrchr(modulePath, '\\');
        char* lastSlash = std::strrchr(modulePath, '/');
        if (!lastBackslash || (lastSlash && lastSlash > lastBackslash))
            lastBackslash = lastSlash;
        if (lastBackslash)
            lastBackslash[1] = '\0';
        else
            modulePath[0] = '\0';

        CopyCString(path, sizeof(path), modulePath);
        const std::size_t used = std::strlen(path);
        if (used < sizeof(path) - 1)
        {
            CopyCString(path + used, sizeof(path) - used, requestedName);
        }
        return path;
    }

    void LoadSettings(Settings& settings, const char* iniFileName)
    {
        const char* iniPath = GetConfigurationPath(iniFileName);
        settings.m_freezeTime = ReadIniBool("General", "FreezeTime", true, iniPath);
        settings.m_hideHud = ReadIniBool("General", "HideHUD", true, iniPath);
        settings.m_freezeEffectsOnStart = ReadIniBool("General", "FreezeEffectsOnStart", true, iniPath);
        settings.m_showHelpOnStart = ReadIniBool("General", "ShowHelpOnStart", true, iniPath);
        settings.m_helpFontSize = ReadIniFloat("Interface", "FontSize", 0.45f, iniPath);
        settings.m_minFov = ReadIniFloat("Camera", "MinimumFOV", 10.0f, iniPath);
        settings.m_maxFov = ReadIniFloat("Camera", "MaximumFOV", 150.0f, iniPath);
        settings.m_zoomStep = ReadIniFloat("Camera", "ZoomStep", -1.0f, iniPath);
        if (settings.m_zoomStep < 0.0f)
            settings.m_zoomStep = ReadIniFloat("Camera", "ZoomSpeed", 30.0f, iniPath) / 60.0f;
        settings.m_mouseSensitivity = ReadIniFloat("Camera", "MouseSensitivity", 0.0026f, iniPath);
        settings.m_baseMoveSpeed = ReadIniFloat("Camera", "MoveSpeed", 7.0f, iniPath);
        settings.m_fastMoveMultiplier = ReadIniFloat("Camera", "FastMoveMultiplier", 3.0f, iniPath);
        settings.m_slowMoveMultiplier = ReadIniFloat("Camera", "SlowMoveMultiplier", 0.22f, iniPath);
        settings.m_hoursPerPress = static_cast<int>(ReadIniFloat("Environment", "HoursPerPress", 1.0f, iniPath));

        settings.m_helpFontSize = ClampFloat(settings.m_helpFontSize, 0.18f, 0.75f);
        if (settings.m_minFov < 1.0f || settings.m_minFov > 60.0f) settings.m_minFov = 10.0f;
        if (settings.m_maxFov < 40.0f || settings.m_maxFov > 179.0f) settings.m_maxFov = 150.0f;
        if (settings.m_minFov >= settings.m_maxFov) { settings.m_minFov = 10.0f; settings.m_maxFov = 150.0f; }
        if (settings.m_zoomStep < 0.05f || settings.m_zoomStep > 5.0f) settings.m_zoomStep = 0.5f;
        if (settings.m_mouseSensitivity < 0.0001f || settings.m_mouseSensitivity > 0.02f) settings.m_mouseSensitivity = 0.0026f;
        if (settings.m_baseMoveSpeed < 0.1f || settings.m_baseMoveSpeed > 100.0f) settings.m_baseMoveSpeed = 7.0f;
        if (settings.m_fastMoveMultiplier < 1.0f || settings.m_fastMoveMultiplier > 20.0f) settings.m_fastMoveMultiplier = 3.0f;
        if (settings.m_slowMoveMultiplier < 0.01f || settings.m_slowMoveMultiplier > 1.0f) settings.m_slowMoveMultiplier = 0.22f;
        if (settings.m_hoursPerPress < 1 || settings.m_hoursPerPress > 12) settings.m_hoursPerPress = 1;

        settings.m_keyModeToggle = ReadIniKey("ModeToggle", "SC_0x2F", KeyCode::V, iniPath);
        settings.m_keyToggleTimePlayback = ReadIniKey("ToggleTimePlayback", "SC_0x3D", KeyCode::F3, iniPath);
        settings.m_keyWideScreen = ReadIniKey("Widescreen", "SC_0x0F", KeyCode::Tab, iniPath);
        settings.m_keyZoomIn = ReadIniKey("ZoomIn", "SC_0x1A", KeyCode::LeftBracket, iniPath);
        settings.m_keyZoomOut = ReadIniKey("ZoomOut", "SC_0x1B", KeyCode::RightBracket, iniPath);
        settings.m_keyResetZoom = ReadIniKey("ResetZoom", "SC_0x19", KeyCode::P, iniPath);
        settings.m_keyShowHelp = ReadIniKey("ToggleHelp", "SC_0x3B", KeyCode::F1, iniPath);
        settings.m_keyFreezeEffects = ReadIniKey("ToggleEffectFreeze", "SC_0x3C", KeyCode::F2, iniPath);
        settings.m_keyCapturePhoto = ReadIniKey("TakePhoto", "SC_0x58", KeyCode::F12, iniPath);
        settings.m_keyAscend = ReadIniKey("Ascend", "SC_0x39", KeyCode::Space, iniPath);
        settings.m_keyDescend = ReadIniKey("Descend", "SC_0x2E", KeyCode::C, iniPath);
        settings.m_keyForward = ReadIniKey("MoveForward", "SC_0x11", KeyCode::W, iniPath);
        settings.m_keyBackward = ReadIniKey("MoveBackward", "SC_0x1F", KeyCode::S, iniPath);
        settings.m_keyLeft = ReadIniKey("MoveLeft", "SC_0x1E", KeyCode::A, iniPath);
        settings.m_keyRight = ReadIniKey("MoveRight", "SC_0x20", KeyCode::D, iniPath);
        settings.m_keyMoveFast = ReadIniKey("MoveFast", "SC_0x200", KeyCode::Shift, iniPath);
        settings.m_keyMoveSlow = ReadIniKey("MoveSlow", "SC_0x201", KeyCode::Control, iniPath);
        settings.m_keyHourDown = ReadIniKey("TimeHourDecrease", "SC_0x0C", KeyCode::Minus, iniPath);
        settings.m_keyHourUp = ReadIniKey("TimeHourIncrease", "SC_0x0D", KeyCode::Equals, iniPath);
    }

    std::string GetKeyLabel(int scanCode)
    {
        // These are physical US-layout labels for Set-1 scan codes. Do not use
        // GetKeyNameTextA here: it translates key names according to the current
        // Windows layout, which made the in-game help change after Alt+Shift.
        if (scanCode == KeyCode::Shift || scanCode == KeyCode::LeftShift || scanCode == KeyCode::RightShift) return "Shift";
        if (scanCode == KeyCode::Control || scanCode == KeyCode::LeftControl || scanCode == KeyCode::RightControl) return "Ctrl";
        if (scanCode == KeyCode::Alt || scanCode == KeyCode::LeftAlt || scanCode == KeyCode::RightAlt) return "Alt";
        if (scanCode == KeyCode::NumPlus) return "Num+";
        if (scanCode == KeyCode::NumMinus) return "Num-";

        switch (scanCode)
        {
        case 0x001: return "Esc";
        case 0x002: return "1"; case 0x003: return "2"; case 0x004: return "3";
        case 0x005: return "4"; case 0x006: return "5"; case 0x007: return "6";
        case 0x008: return "7"; case 0x009: return "8"; case 0x00A: return "9";
        case 0x00B: return "0"; case 0x00C: return "-"; case 0x00D: return "=";
        case 0x00E: return "Backspace"; case 0x00F: return "Tab";
        case 0x010: return "Q"; case 0x011: return "W"; case 0x012: return "E";
        case 0x013: return "R"; case 0x014: return "T"; case 0x015: return "Y";
        case 0x016: return "U"; case 0x017: return "I"; case 0x018: return "O";
        case 0x019: return "P"; case 0x01A: return "["; case 0x01B: return "]";
        case 0x01C: return "Enter"; case 0x01E: return "A"; case 0x01F: return "S";
        case 0x020: return "D"; case 0x021: return "F"; case 0x022: return "G";
        case 0x023: return "H"; case 0x024: return "J"; case 0x025: return "K";
        case 0x026: return "L"; case 0x027: return ";"; case 0x028: return "'";
        case 0x029: return "`"; case 0x02B: return "\\"; case 0x02C: return "Z";
        case 0x02D: return "X"; case 0x02E: return "C"; case 0x02F: return "V";
        case 0x030: return "B"; case 0x031: return "N"; case 0x032: return "M";
        case 0x033: return ","; case 0x034: return "."; case 0x035: return "/";
        case 0x037: return "Num*"; case 0x039: return "Space"; case 0x03A: return "CapsLock";
        case 0x03B: return "F1"; case 0x03C: return "F2"; case 0x03D: return "F3";
        case 0x03E: return "F4"; case 0x03F: return "F5"; case 0x040: return "F6";
        case 0x041: return "F7"; case 0x042: return "F8"; case 0x043: return "F9";
        case 0x044: return "F10"; case 0x045: return "NumLock"; case 0x046: return "ScrollLock";
        case 0x047: return "Num7"; case 0x048: return "Num8"; case 0x049: return "Num9";
        case 0x04B: return "Num4"; case 0x04C: return "Num5"; case 0x04D: return "Num6";
        case 0x04F: return "Num1"; case 0x050: return "Num2"; case 0x051: return "Num3";
        case 0x052: return "Num0"; case 0x053: return "Num."; case 0x056: return "\\|";
        case 0x057: return "F11"; case 0x058: return "F12";
        case 0x11C: return "NumEnter"; case 0x135: return "Num/";
        case 0x147: return "Home"; case 0x148: return "Up"; case 0x149: return "PgUp";
        case 0x14B: return "Left"; case 0x14D: return "Right"; case 0x14F: return "End";
        case 0x150: return "Down"; case 0x151: return "PgDn"; case 0x152: return "Insert";
        case 0x153: return "Delete";
        default: break;
        }

        char label[24] = {};
        std::snprintf(label, sizeof(label), "SC_0x%03X", scanCode);
        return label;
    }

    std::array<std::string, 6> BuildCommonCameraHelpLines(const Settings& settings)
    {
        return {{
            "Alt+" + GetKeyLabel(settings.m_keyModeToggle) + ": toggle mode | " +
                GetKeyLabel(settings.m_keyModeToggle) + ": FREE/FOLLOW | " +
                GetKeyLabel(settings.m_keyToggleTimePlayback) + ": play/pause time",
            "Esc: open/close game menu | " + GetKeyLabel(settings.m_keyForward) + "/" +
                GetKeyLabel(settings.m_keyLeft) + "/" + GetKeyLabel(settings.m_keyBackward) + "/" +
                GetKeyLabel(settings.m_keyRight) + " / arrows: move",
            GetKeyLabel(settings.m_keyAscend) + " / PgUp: rise    " + GetKeyLabel(settings.m_keyDescend) + " / PgDn: lower",
            "Mouse: look    LMB: lock X    RMB: lock Y",
            GetKeyLabel(settings.m_keyMoveFast) + ": faster    " + GetKeyLabel(settings.m_keyMoveSlow) + ": slower",
            "Zoom: hold " + GetKeyLabel(settings.m_keyZoomIn) + "/" +
                GetKeyLabel(settings.m_keyZoomOut) + " or Num+/- | " +
                GetKeyLabel(settings.m_keyResetZoom) + ": reset zoom"
        }};
    }

    std::string BuildCameraModeHelpLine(bool freeCameraEnabled)
    {
        return std::string("Camera mode: ") + (freeCameraEnabled ? "FREE" : "PLAYER FOLLOW");
    }

    std::string BuildCameraTimeHelpLine(bool freeCameraEnabled, bool timePlaybackEnabled, bool gameMenuActive)
    {
        const char* timeStatus = gameMenuActive ? "MENU" : (timePlaybackEnabled ? "PLAYING" : "PAUSED");
        return std::string("Camera mode: ") + (freeCameraEnabled ? "FREE" : "PLAYER FOLLOW") +
            " | Time: " + timeStatus;
    }

    std::string BuildHudHelpLine(bool hudHidden)
    {
        return std::string("HUD: ") + (hudHidden ? "hidden" : "normal");
    }

    std::string BuildHudTimeHelpLine(bool hudHidden, bool timeFrozen)
    {
        return std::string("HUD: ") + (hudHidden ? "hidden" : "normal") +
            " | time: " + (timeFrozen ? "frozen" : "running");
    }

    void PlayModeActivationSound()
    {
        // Shared activation cue; called once when entering screenshot mode in III, VC and SA.
        MessageBeep(MB_OK);
    }

    float ClampFloat(float value, float minimum, float maximum)
    {
        return std::max(minimum, std::min(maximum, value));
    }

    float UpdateContinuousCameraFov(float currentFov, float initialFov, const Settings& settings,
        bool zoomInDown, bool zoomOutDown, bool resetPressed)
    {
        // Mirror the working III/VC behavior: the configured step is applied on
        // each rendered update while a key is held, avoiding SA's old delta-time
        // scaling that could make the FOV step effectively imperceptible.
        if (resetPressed)
            currentFov = initialFov;
        else if (zoomInDown && !zoomOutDown)
            currentFov -= settings.m_zoomStep;
        else if (zoomOutDown && !zoomInDown)
            currentFov += settings.m_zoomStep;
        return ClampFloat(currentFov, settings.m_minFov, settings.m_maxFov);
    }

    bool InputTracker::Initialize()
    {
        const bool foreground = IsCurrentProcessForeground();
        s_inputProcessForeground.store(foreground, std::memory_order_relaxed);
        if (!foreground)
        {
            ClearPhysicalKeyStates();
            m_previousKeys.fill(false);
            s_altChordSuppressionActive.store(false, std::memory_order_relaxed);
            s_gameMenuInputActive.store(false, std::memory_order_relaxed);
            s_nativeGameMenuObservedActive.store(false, std::memory_order_relaxed);
            s_escapeKeyUpWasForwarded.store(false, std::memory_order_relaxed);
        }

        // A global low-level hook is only needed while the plugin captures game input
        // or briefly while Alt is held for the Alt+V mode-toggle chord. It is not
        // installed during ordinary gameplay/loading, avoiding a permanent system-wide
        // keyboard hook and its impact on the Windows input path.
        const bool shouldHook = foreground &&
            (s_keyboardCaptureActive.load(std::memory_order_relaxed) || IsAnyAltDownNow());
        if (shouldHook)
            return EnsureKeyboardHookInstalled();

        if (s_keyboardHook)
        {
            UnhookWindowsHookEx(s_keyboardHook);
            s_keyboardHook = nullptr;
            s_nextKeyboardHookAttemptTick = 0;
            ClearPhysicalKeyStates();
            s_altChordSuppressionActive.store(false, std::memory_order_relaxed);
        }
        return false;
    }

    void InputTracker::Shutdown()
    {
        s_keyboardCaptureActive.store(false, std::memory_order_relaxed);
        s_gameMenuInputActive.store(false, std::memory_order_relaxed);
        s_nativeGameMenuObservedActive.store(false, std::memory_order_relaxed);
        s_escapeKeyUpWasForwarded.store(false, std::memory_order_relaxed);
        if (s_keyboardHook)
        {
            UnhookWindowsHookEx(s_keyboardHook);
            s_keyboardHook = nullptr;
        }
        s_nextKeyboardHookAttemptTick = 0;
        ClearPhysicalKeyStates();
        Reset();
    }

    bool InputTracker::IsKeyDown(int scanCode) const
    {
        if (scanCode == KeyCode::Shift)
            return IsPhysicalKeyDown(KeyCode::LeftShift) || IsPhysicalKeyDown(KeyCode::RightShift);
        if (scanCode == KeyCode::Control)
            return IsPhysicalKeyDown(KeyCode::LeftControl) || IsPhysicalKeyDown(KeyCode::RightControl);
        if (scanCode == KeyCode::Alt)
            return IsPhysicalKeyDown(KeyCode::LeftAlt) || IsPhysicalKeyDown(KeyCode::RightAlt);
        return IsPhysicalKeyDown(scanCode);
    }

    bool InputTracker::WasKeyPressed(int scanCode)
    {
        if (scanCode < 0 || scanCode >= static_cast<int>(m_previousKeys.size()))
            return false;
        const bool down = IsKeyDown(scanCode);
        const bool pressed = down && !m_previousKeys[static_cast<std::size_t>(scanCode)];
        m_previousKeys[static_cast<std::size_t>(scanCode)] = down;
        return pressed;
    }

    bool InputTracker::IsMouseButtonDown(int virtualKey) const
    {
        if (virtualKey != VK_LBUTTON && virtualKey != VK_RBUTTON && virtualKey != VK_MBUTTON)
            return false;
        return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    }

    void InputTracker::SetCaptureMode(bool active)
    {
        s_keyboardCaptureActive.store(active, std::memory_order_relaxed);
        if (active && s_inputProcessForeground.load(std::memory_order_relaxed))
            EnsureKeyboardHookInstalled();
        if (!active)
        {
            s_gameMenuInputActive.store(false, std::memory_order_relaxed);
            s_nativeGameMenuObservedActive.store(false, std::memory_order_relaxed);
            s_escapeKeyUpWasForwarded.store(false, std::memory_order_relaxed);
        }
        // When leaving capture mode, Initialize() will remove the hook as soon as
        // Alt is released, preserving the key-up edge of the Alt+V chord.
    }

    void InputTracker::SetModeToggleKey(int scanCode)
    {
        if (scanCode < 0 || scanCode >= KeyCode::ScanCodeCount)
            scanCode = -1;
        s_altChordKeyScanCode.store(scanCode, std::memory_order_relaxed);
        s_altChordSuppressionActive.store(false, std::memory_order_relaxed);
    }

    bool InputTracker::IsGameMenuInputActive() const
    {
        return s_gameMenuInputActive.load(std::memory_order_relaxed);
    }

    void InputTracker::SetGameMenuInputActive(bool active)
    {
        if (active)
        {
            s_nativeGameMenuObservedActive.store(true, std::memory_order_relaxed);
            s_gameMenuInputActive.store(true, std::memory_order_relaxed);
        }
        else if (s_nativeGameMenuObservedActive.exchange(false, std::memory_order_relaxed))
        {
            // The menu really was active and the native game now reports it closed
            // (for example, Resume selected with Enter instead of a second Esc).
            s_gameMenuInputActive.store(false, std::memory_order_relaxed);
            s_escapeKeyUpWasForwarded.store(false, std::memory_order_relaxed);
        }
    }

    void InputTracker::Reset()
    {
        m_previousKeys.fill(false);
    }

    Vector3 ForwardFromYawPitch(float yaw, float pitch)
    {
        const float horizontal = std::cos(pitch);
        return {horizontal * std::cos(yaw), horizontal * std::sin(yaw), std::sin(pitch)};
    }

    Vector3 RightFromYaw(float yaw)
    {
        return {std::sin(yaw), -std::cos(yaw), 0.0f};
    }

    Vector3 Normalize(const Vector3& vector, const Vector3& fallback)
    {
        const float length = std::sqrt(vector.x * vector.x + vector.y * vector.y + vector.z * vector.z);
        if (length < 0.001f || !std::isfinite(length))
            return fallback;
        return {vector.x / length, vector.y / length, vector.z / length};
    }
}
