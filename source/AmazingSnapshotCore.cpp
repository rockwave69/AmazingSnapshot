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
#include <cwchar>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace AmazingSnapshot
{
    namespace
    {
        void CopyWideString(wchar_t* destination, std::size_t capacity, const wchar_t* source)
        {
            if (!destination || capacity == 0)
                return;
            if (!source)
            {
                destination[0] = L'\0';
                return;
            }
            std::size_t length = std::wcslen(source);
            if (length >= capacity)
                length = capacity - 1;
            if (length > 0)
                std::wmemcpy(destination, source, length);
            destination[length] = L'\0';
        }

        float ReadIniFloat(const wchar_t* section, const wchar_t* key, float defaultValue,
                           const wchar_t* iniPath)
        {
            wchar_t defaultText[48] = {};
            wchar_t valueText[64] = {};
            std::swprintf(defaultText, sizeof(defaultText) / sizeof(defaultText[0]), L"%.6f", defaultValue);
            GetPrivateProfileStringW(section, key, defaultText, valueText,
                static_cast<DWORD>(sizeof(valueText) / sizeof(valueText[0])), iniPath);

            wchar_t* end = nullptr;
            const float value = std::wcstof(valueText, &end);
            if (end == valueText || !std::isfinite(value))
                return defaultValue;
            while (*end && std::iswspace(*end))
                ++end;
            return *end == L'\0' ? value : defaultValue;
        }

        bool ReadIniBool(const wchar_t* section, const wchar_t* key, bool defaultValue,
                         const wchar_t* iniPath)
        {
            wchar_t defaultText[8] = {};
            wchar_t valueText[32] = {};
            std::swprintf(defaultText, sizeof(defaultText) / sizeof(defaultText[0]), L"%d", defaultValue ? 1 : 0);
            GetPrivateProfileStringW(section, key, defaultText, valueText,
                static_cast<DWORD>(sizeof(valueText) / sizeof(valueText[0])), iniPath);

            std::wstring value(valueText);
            while (!value.empty() && std::iswspace(value.front()))
                value.erase(value.begin());
            while (!value.empty() && std::iswspace(value.back()))
                value.pop_back();
            for (wchar_t& character : value)
                character = static_cast<wchar_t>(std::towlower(character));

            if (value == L"1" || value == L"true" || value == L"yes" || value == L"on")
                return true;
            if (value == L"0" || value == L"false" || value == L"no" || value == L"off")
                return false;
            return defaultValue;
        }

        constexpr std::size_t PHYSICAL_KEY_COUNT = KeyCode::ScanCodeCount;
        std::array<std::atomic_bool, PHYSICAL_KEY_COUNT> s_abPhysicalKeyDown{};
        std::atomic_bool s_bKeyboardCaptureActive{ false };
        std::atomic<int> s_nAltChordKeyScanCode{ -1 };
        std::atomic_bool s_bAltChordSuppressionActive{ false };
        std::atomic_bool s_bGameMenuInputActive{ false };
        std::atomic_bool s_bNativeGameMenuObservedActive{ false };
        std::atomic_bool s_bEscapeKeyUpWasForwarded{ false };
        HHOOK s_pKeyboardHook = nullptr;
        DWORD s_nNextKeyboardHookAttemptTick = 0;
        std::atomic_bool s_bInputProcessForeground{ false };
        std::atomic<HWND> s_hExpectedForegroundWindow{ nullptr };

        bool IsCurrentProcessForeground()
        {
            const HWND foregroundWindow = GetForegroundWindow();
            if (!foregroundWindow)
            {
                s_hExpectedForegroundWindow.store(nullptr, std::memory_order_relaxed);
                return false;
            }

            DWORD foregroundProcessId = 0;
            GetWindowThreadProcessId(foregroundWindow, &foregroundProcessId);
            const bool isGameForeground = foregroundProcessId == GetCurrentProcessId();
            s_hExpectedForegroundWindow.store(isGameForeground ? foregroundWindow : nullptr,
                std::memory_order_relaxed);
            return isGameForeground;
        }

        void ClearPhysicalKeyStates()
        {
            for (auto& state : s_abPhysicalKeyDown)
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
            return s_abPhysicalKeyDown[KeyCode::LeftAlt].load(std::memory_order_relaxed) ||
                   s_abPhysicalKeyDown[KeyCode::RightAlt].load(std::memory_order_relaxed);
        }

        bool IsControlPhysicallyDownFromHookState()
        {
            return s_abPhysicalKeyDown[KeyCode::LeftControl].load(std::memory_order_relaxed) ||
                   s_abPhysicalKeyDown[KeyCode::RightControl].load(std::memory_order_relaxed);
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
                return CallNextHookEx(s_pKeyboardHook, code, message, data);

            // The hook is installed only for Alt+mode-toggle detection or while the
            // screenshot mode captures input. Confirm the foreground HWND cheaply on
            // every hook event so that a minimized/background GTA cannot swallow keys
            // intended for another application. Avoid GetWindowThreadProcessId here:
            // it is only needed when sampling focus from the render callback.
            const HWND expectedWindow = s_hExpectedForegroundWindow.load(std::memory_order_relaxed);
            if (!s_bInputProcessForeground.load(std::memory_order_relaxed) || !expectedWindow)
                return CallNextHookEx(s_pKeyboardHook, code, message, data);
            if (GetForegroundWindow() != expectedWindow)
            {
                // Focus may change while a minimized game stops drawing. Do not
                // swallow the other app's input, and clear held keys so they cannot
                // stick when GTA regains focus before the next render callback.
                s_bInputProcessForeground.store(false, std::memory_order_relaxed);
                s_hExpectedForegroundWindow.store(nullptr, std::memory_order_relaxed);
                ClearPhysicalKeyStates();
                s_bAltChordSuppressionActive.store(false, std::memory_order_relaxed);
                return CallNextHookEx(s_pKeyboardHook, code, message, data);
            }

            if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN &&
                message != WM_KEYUP && message != WM_SYSKEYUP)
                return CallNextHookEx(s_pKeyboardHook, code, message, data);

            const KBDLLHOOKSTRUCT* keyEvent = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
            const int scanCode = GetHookScanCode(*keyEvent);
            const bool isDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            bool wasDown = false;
            if (scanCode >= 0 && scanCode < static_cast<int>(s_abPhysicalKeyDown.size()))
            {
                wasDown = s_abPhysicalKeyDown[static_cast<std::size_t>(scanCode)].load(std::memory_order_relaxed);
                s_abPhysicalKeyDown[static_cast<std::size_t>(scanCode)].store(
                    isDown, std::memory_order_relaxed);
            }

            // Alt+ModeToggle belongs to this plugin. Consume both edges so GTA
            // never receives the V that would otherwise switch the native camera.
            const int chordKey = s_nAltChordKeyScanCode.load(std::memory_order_relaxed);
            if (chordKey >= 0 && scanCode == chordKey)
            {
                if (isDown && IsAltPhysicallyDownFromHookState())
                {
                    s_bAltChordSuppressionActive.store(true, std::memory_order_relaxed);
                    return 1;
                }
                if (!isDown && s_bAltChordSuppressionActive.exchange(false, std::memory_order_relaxed))
                    return 1;
            }

            if (s_bKeyboardCaptureActive.load(std::memory_order_relaxed))
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
                        s_bGameMenuInputActive.store(true, std::memory_order_relaxed);
                        s_bEscapeKeyUpWasForwarded.store(true, std::memory_order_relaxed);
                    }
                    if (isDown || s_bEscapeKeyUpWasForwarded.exchange(false, std::memory_order_relaxed))
                        return CallNextHookEx(s_pKeyboardHook, code, message, data);
                }

                // Once Esc opens the menu, allow its navigation keys, but keep
                // replay/function/gameplay hotkeys blocked to avoid collisions.
                if (s_bGameMenuInputActive.load(std::memory_order_relaxed) &&
                    IsMenuNavigationKey(keyEvent->vkCode))
                {
                    return CallNextHookEx(s_pKeyboardHook, code, message, data);
                }

                // Preserve Windows shortcuts; all other game keyboard input is captured.
                if (!IsSystemShortcut(*keyEvent))
                    return 1;
            }
            return CallNextHookEx(s_pKeyboardHook, code, message, data);
        }

        bool EnsureKeyboardHookInstalled()
        {
            const HWND expectedWindow = s_hExpectedForegroundWindow.load(std::memory_order_relaxed);
            if (!s_bInputProcessForeground.load(std::memory_order_relaxed) || !expectedWindow ||
                GetForegroundWindow() != expectedWindow)
                return false;

            if (s_pKeyboardHook)
                return true;

            const DWORD now = GetTickCount();
            if (s_nNextKeyboardHookAttemptTick != 0 && now < s_nNextKeyboardHookAttemptTick)
                return false;
            s_nNextKeyboardHookAttemptTick = now + 1000;

            HMODULE module = nullptr;
            const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
            if (!GetModuleHandleExA(flags,
                    reinterpret_cast<LPCSTR>(&KeyboardHookProc), &module))
            {
                OutputDebugStringA("AmazingSnapshot: could not resolve keyboard hook module; retrying.\n");
                return false;
            }

            s_pKeyboardHook = SetWindowsHookExA(WH_KEYBOARD_LL, KeyboardHookProc, module, 0);
            if (!s_pKeyboardHook)
            {
                OutputDebugStringA("AmazingSnapshot: physical keyboard hook could not be installed; retrying.\n");
                return false;
            }
            ClearPhysicalKeyStates();
            // Alt was usually pressed in the frame that caused this lazy install,
            // before the hook could observe its WM_SYSKEYDOWN edge. Seed modifier
            // state once so Alt+V and Ctrl/Shift menu chords still work immediately.
            s_abPhysicalKeyDown[KeyCode::LeftAlt].store(IsDownNow(VK_LMENU), std::memory_order_relaxed);
            s_abPhysicalKeyDown[KeyCode::RightAlt].store(IsDownNow(VK_RMENU), std::memory_order_relaxed);
            if (!IsAltPhysicallyDownFromHookState() && IsDownNow(VK_MENU))
                s_abPhysicalKeyDown[KeyCode::LeftAlt].store(true, std::memory_order_relaxed);
            s_abPhysicalKeyDown[KeyCode::LeftControl].store(IsDownNow(VK_LCONTROL), std::memory_order_relaxed);
            s_abPhysicalKeyDown[KeyCode::RightControl].store(IsDownNow(VK_RCONTROL), std::memory_order_relaxed);
            s_abPhysicalKeyDown[KeyCode::LeftShift].store(IsDownNow(VK_LSHIFT), std::memory_order_relaxed);
            s_abPhysicalKeyDown[KeyCode::RightShift].store(IsDownNow(VK_RSHIFT), std::memory_order_relaxed);
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
            if (key == "NUM_MULTIPLY" || key == "NUMPADMULTIPLY" || key == "NUMPADSTAR" || key == "NUMPADASTERISK") return KeyCode::NumMultiply;
            if (key == "NUM_DIVIDE" || key == "NUMPADDIVIDE" || key == "NUMPADSLASH") return KeyCode::NumDivide;
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

        int ReadIniKey(const wchar_t* name, const wchar_t* defaultValue, int fallback,
                       const wchar_t* iniPath)
        {
            wchar_t wideValue[64] = {};
            GetPrivateProfileStringW(L"Keys", name, defaultValue, wideValue,
                static_cast<DWORD>(sizeof(wideValue) / sizeof(wideValue[0])), iniPath);

            // Key labels are deliberately ASCII tokens (SC_0xNN, W, F12, etc.);
            // the INI path itself is Unicode and must not be round-tripped via ACP.
            char value[64] = {};
            for (std::size_t i = 0; i < (sizeof(value) - 1) && wideValue[i] != L'\0'; ++i)
            {
                if (wideValue[i] > 0x7F)
                    return fallback;
                value[i] = static_cast<char>(wideValue[i]);
            }
            return ParseScanCode(value, fallback);
        }

        bool IsPhysicalKeyDown(int scanCode)
        {
            if (scanCode < 0 || scanCode >= static_cast<int>(PHYSICAL_KEY_COUNT) ||
                !s_bInputProcessForeground.load(std::memory_order_relaxed))
                return false;

            // While capture is active, the low-level hook deliberately swallows
            // gameplay key events. GetAsyncKeyState is not reliable for every
            // swallowed event, so use the physical state captured by the hook as
            // the source of truth. This is essential for camera movement and Esc
            // to keep working while other GTA hotkeys are blocked.
            if ((s_bKeyboardCaptureActive.load(std::memory_order_relaxed) ||
                 s_bAltChordSuppressionActive.load(std::memory_order_relaxed)) && s_pKeyboardHook)
            {
                // The hook also swallows the configured key for Alt+mode-toggle
                // before capture mode starts, so use the hook state for that chord
                // too; GetAsyncKeyState may not report a swallowed key reliably.
                return s_abPhysicalKeyDown[static_cast<std::size_t>(scanCode)].load(std::memory_order_relaxed);
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

            if (!s_pKeyboardHook)
                return false;
            return s_abPhysicalKeyDown[static_cast<std::size_t>(scanCode)].load(std::memory_order_relaxed);
        }

        bool EnsureDirectoryExists(const std::wstring& directory)
        {
            if (CreateDirectoryW(directory.c_str(), nullptr))
                return true;
            if (GetLastError() != ERROR_ALREADY_EXISTS)
                return false;
            const DWORD attributes = GetFileAttributesW(directory.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }

        bool IsExecutableProtection(DWORD protection)
        {
            if ((protection & PAGE_GUARD) != 0)
                return false;

            switch (protection & 0xFFu)
            {
            case PAGE_EXECUTE:
            case PAGE_EXECUTE_READ:
            case PAGE_EXECUTE_READWRITE:
            case PAGE_EXECUTE_WRITECOPY:
                return true;
            default:
                return false;
            }
        }

        struct GameImageInfo
        {
            HMODULE m_pModule = nullptr;
            std::uintptr_t m_nLoadedBase = 0;
            std::uintptr_t m_nPreferredBase = 0;
            std::uintptr_t m_nImageSize = 0;
        };

        bool GetGameImageInfo(GameImageInfo& output)
        {
            HMODULE gameModule = GetModuleHandleW(nullptr);
            if (!gameModule)
                return false;

            IMAGE_DOS_HEADER dosHeader{};
            SIZE_T bytesRead = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), gameModule, &dosHeader,
                sizeof(dosHeader), &bytesRead) || bytesRead != sizeof(dosHeader) ||
                dosHeader.e_magic != IMAGE_DOS_SIGNATURE || dosHeader.e_lfanew <= 0)
            {
                return false;
            }

            // GTA III and Vice City targets are 32-bit PE executables. Read the
            // preferred image base so signature addresses remain valid if the EXE
            // is ever relocated by the loader.
            IMAGE_NT_HEADERS32 ntHeaders{};
            const auto* ntHeaderAddress = reinterpret_cast<const unsigned char*>(gameModule) + dosHeader.e_lfanew;
            if (!ReadProcessMemory(GetCurrentProcess(), ntHeaderAddress, &ntHeaders,
                sizeof(ntHeaders), &bytesRead) || bytesRead != sizeof(ntHeaders) ||
                ntHeaders.Signature != IMAGE_NT_SIGNATURE ||
                ntHeaders.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
                ntHeaders.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
                ntHeaders.OptionalHeader.SizeOfImage == 0)
            {
                return false;
            }

            output.m_pModule = gameModule;
            output.m_nLoadedBase = reinterpret_cast<std::uintptr_t>(gameModule);
            output.m_nPreferredBase = static_cast<std::uintptr_t>(ntHeaders.OptionalHeader.ImageBase);
            output.m_nImageSize = static_cast<std::uintptr_t>(ntHeaders.OptionalHeader.SizeOfImage);
            return true;
        }

        bool GetLoadedGameAddress(const GameImageInfo& image, std::uintptr_t preferredVirtualAddress,
            std::uintptr_t& outputAddress)
        {
            if (preferredVirtualAddress < image.m_nPreferredBase)
                return false;

            const std::uintptr_t rva = preferredVirtualAddress - image.m_nPreferredBase;
            if (rva > image.m_nImageSize || image.m_nImageSize - rva < 5u)
                return false;

            const std::uintptr_t address = image.m_nLoadedBase + rva;
            MEMORY_BASIC_INFORMATION memoryInfo{};
            if (VirtualQuery(reinterpret_cast<const void*>(address), &memoryInfo,
                sizeof(memoryInfo)) != sizeof(memoryInfo) ||
                memoryInfo.State != MEM_COMMIT || memoryInfo.Type != MEM_IMAGE ||
                memoryInfo.AllocationBase != image.m_pModule ||
                !IsExecutableProtection(memoryInfo.Protect))
            {
                return false;
            }

            outputAddress = address;
            return true;
        }

        bool TryReadExternalBranchTarget(const GameImageInfo& image, std::uintptr_t preferredVirtualAddress,
            std::uintptr_t& outputTarget)
        {
            std::uintptr_t hookAddress = 0;
            if (!GetLoadedGameAddress(image, preferredVirtualAddress, hookAddress))
                return false;

            unsigned char jump[5] = {};
            SIZE_T bytesRead = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(hookAddress),
                jump, sizeof(jump), &bytesRead) || bytesRead != sizeof(jump) || (jump[0] != 0xE9 && jump[0] != 0xE8))
            {
                return false;
            }

            std::int32_t relativeOffset = 0;
            std::memcpy(&relativeOffset, jump + 1, sizeof(relativeOffset));
            const std::uintptr_t target = hookAddress + sizeof(jump) +
                static_cast<std::uintptr_t>(static_cast<std::intptr_t>(relativeOffset));

            MEMORY_BASIC_INFORMATION targetInfo{};
            if (VirtualQuery(reinterpret_cast<const void*>(target), &targetInfo,
                sizeof(targetInfo)) != sizeof(targetInfo) ||
                targetInfo.State != MEM_COMMIT || targetInfo.Type != MEM_IMAGE ||
                targetInfo.AllocationBase == image.m_pModule ||
                !IsExecutableProtection(targetInfo.Protect))
            {
                return false;
            }

            outputTarget = target;
            return true;
        }

        bool DoPortableGtaHookSitesMatch(const GameImageInfo& image,
            const std::uintptr_t* preferredHookAddresses, std::size_t hookCount)
        {
            if (!preferredHookAddresses || hookCount < 2)
                return false;

            std::uintptr_t expectedTarget = 0;
            for (std::size_t index = 0; index < hookCount; ++index)
            {
                std::uintptr_t target = 0;
                if (!TryReadExternalBranchTarget(image, preferredHookAddresses[index], target))
                    return false;

                if (index == 0)
                    expectedTarget = target;
                else if (target != expectedTarget)
                    return false;
            }
            return true;
        }

        bool IsPortableGtaRedirectActive()
        {
            GameImageInfo image{};
            if (!GetGameImageInfo(image))
                return false;

            // PortableGTA's published source patches these call sites to the same
            // getUserFilesDir() function. Sites may use CALL rel32 (E8) or JMP rel32 (E9). This behavior-based signature does not
            // depend on whether the ASI/DLL has been renamed. It also avoids a false
            // positive when a file is present but never loaded by the ASI loader.
            // Addresses are the GTA III 1.0 US / Vice City 1.0 US sites documented
            // by portablegta.cpp (GTAmodding/miscmods).
            static const std::uintptr_t gtaIIIHookSites[] =
            {
                0x479080u, 0x5811DDu, 0x591EDDu
            };
            static const std::uintptr_t viceCityHookSites[] =
            {
                0x48E020u, 0x61D8CAu
            };

            return DoPortableGtaHookSitesMatch(image, gtaIIIHookSites,
                       sizeof(gtaIIIHookSites) / sizeof(gtaIIIHookSites[0])) ||
                DoPortableGtaHookSitesMatch(image, viceCityHookSites,
                       sizeof(viceCityHookSites) / sizeof(viceCityHookSites[0]));
        }

        bool GetGameExecutableDirectory(std::wstring& outputDirectory)
        {
            wchar_t executablePath[MAX_PATH] = {};
            const DWORD length = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
            if (length == 0 || length >= MAX_PATH)
                return false;

            wchar_t* lastBackslash = std::wcsrchr(executablePath, L'\\');
            wchar_t* lastSlash = std::wcsrchr(executablePath, L'/');
            if (!lastBackslash || (lastSlash && lastSlash > lastBackslash))
                lastBackslash = lastSlash;
            if (!lastBackslash)
                return false;

            *lastBackslash = L'\0';
            outputDirectory.assign(executablePath);
            return !outputDirectory.empty();
        }
        bool GetPortableGalleryPath(const std::wstring& gameDirectory, std::wstring& outputGalleryPath)
        {
            const std::wstring userFilesPath = gameDirectory + L"\\userfiles";
            if (!EnsureDirectoryExists(userFilesPath))
                return false;

            outputGalleryPath = userFilesPath + L"\\Gallery";
            return EnsureDirectoryExists(outputGalleryPath);
        }

        bool GetDocumentsGalleryPath(const wchar_t* gameUserFilesDirectory, std::wstring& outputGalleryPath)
        {
            if (!gameUserFilesDirectory || gameUserFilesDirectory[0] == L'\0')
                return false;

            wchar_t documentsPath[MAX_PATH] = {};
            if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL | CSIDL_FLAG_CREATE,
                nullptr, SHGFP_TYPE_CURRENT, documentsPath)))
            {
                return false;
            }

            const std::wstring userFilesPath = std::wstring(documentsPath) + L"\\" + gameUserFilesDirectory;
            if (!EnsureDirectoryExists(userFilesPath))
                return false;

            outputGalleryPath = userFilesPath + L"\\Gallery";
            return EnsureDirectoryExists(outputGalleryPath);
        }

        bool GetNextGalleryFilePath(const wchar_t* gameUserFilesDirectory, std::wstring& outputPath)
        {
            std::wstring gameDirectory;
            std::wstring galleryPath;
            if (GetGameExecutableDirectory(gameDirectory) && IsPortableGtaRedirectActive())
            {
                // Portable GTA redirects game user files to <game root>\\userfiles.
                // Keep screenshots in the same per-installation tree so portable
                // copies do not leak photos into a shared Documents folder.
                if (!GetPortableGalleryPath(gameDirectory, galleryPath))
                    return false;
            }
            else
            {
                // Preserve the stock location for regular installations.
                if (!GetDocumentsGalleryPath(gameUserFilesDirectory, galleryPath))
                    return false;
            }

            // Match the stock San Andreas gallery naming convention and never overwrite.
            for (unsigned int index = 1; index <= 99999; ++index)
            {
                wchar_t fileName[64] = {};
                std::swprintf(fileName, sizeof(fileName) / sizeof(fileName[0]), L"gallery%u.jpg", index);
                const std::wstring candidate = galleryPath + L"\\" + fileName;
                const DWORD attributes = GetFileAttributesW(candidate.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES)
                {
                    const DWORD error = GetLastError();
                    if (error == ERROR_FILE_NOT_FOUND)
                    {
                        outputPath = candidate;
                        return true;
                    }
                    // Access denied, a vanished directory, and other filesystem
                    // errors are not equivalent to a free filename.
                    return false;
                }
            }
            return false;
        }

        bool EncodeBgrPixelsAsJpeg(const std::wstring& path, UINT width, UINT height,
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

                    saved = bitmap.Save(path.c_str(), &jpegEncoderClsid,
                        &encoderParameters) == Gdiplus::Ok;
                }
            }

            Gdiplus::GdiplusShutdown(gdiplusToken);
            return saved;
        }

        bool CaptureRenderWareFrameToGallery(void* renderWareCamera, const wchar_t* gameUserFilesDirectory,
            std::wstring& savedPath)
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

            std::wstring path;
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
                DeleteFileW(path.c_str());
                return false;
            }
            savedPath = path;
            return true;
        }

    }

    bool SaveRenderWareFrameToGalleryJpeg(void* renderWareCamera, const wchar_t* gameUserFilesDirectory,
        std::wstring& savedPath)
    {
        return CaptureRenderWareFrameToGallery(renderWareCamera, gameUserFilesDirectory, savedPath);
    }

    std::uint64_t ModeSnapshot::ResourceMask(ModeResource resource)
    {
        return static_cast<std::uint64_t>(resource);
    }

    void ModeSnapshot::Reset()
    {
        m_eExitReason = ModeExitReason::None;
        m_fSavedTimeScale = 1.0f;
        m_nSavedPlayerControlState = 0;
        m_bSavedHudVisible = true;
        m_bSavedHudMarkersVisible = true;
        m_bSavedWideScreen = false;
        m_afSavedCameraFOV.fill(70.0f);
        m_fSavedDrawFOV = 70.0f;
        m_bSavedScriptRadarHidden = false;
        m_bSavedAreaNameHidden = false;
        m_bSavedVehicleNameHidden = false;
        m_bSavedClockVisible = true;
        m_aSavedFxManagerUpdateBytes.fill(0);
        m_nSavedCoronasUpdateByte = 0;
        m_nSavedHudDrawOpcode = 0;
        m_nSavedHudDrawAfterFadeOpcode = 0;
        m_bFxManagerBytesCaptured = false;
        m_bCoronasByteCaptured = false;
        m_bHudDrawByteCaptured = false;
        m_bHudDrawAfterFadeByteCaptured = false;
        m_bFxManagerPatchApplied = false;
        m_bCoronasPatchApplied = false;
        m_bHudDrawPatchApplied = false;
        m_bHudDrawAfterFadePatchApplied = false;
        m_nCapturedResources = 0;
        m_nAppliedResources = 0;
        m_nRestoredResources = 0;
    }

    void ModeSnapshot::MarkCaptured(ModeResource resource)
    {
        m_nCapturedResources |= ResourceMask(resource);
    }

    void ModeSnapshot::MarkApplied(ModeResource resource)
    {
        m_nAppliedResources |= ResourceMask(resource);
        m_nRestoredResources &= ~ResourceMask(resource);
    }

    void ModeSnapshot::MarkRestored(ModeResource resource)
    {
        m_nRestoredResources |= ResourceMask(resource);
    }

    bool ModeSnapshot::IsCaptured(ModeResource resource) const
    {
        return (m_nCapturedResources & ResourceMask(resource)) != 0;
    }

    bool ModeSnapshot::WasApplied(ModeResource resource) const
    {
        return (m_nAppliedResources & ResourceMask(resource)) != 0;
    }

    bool ModeSnapshot::NeedsRestore(ModeResource resource) const
    {
        const std::uint64_t mask = ResourceMask(resource);
        return (m_nAppliedResources & mask) != 0 && (m_nRestoredResources & mask) == 0;
    }

    bool ModeSnapshot::HasPendingRestores() const
    {
        return (m_nAppliedResources & ~m_nRestoredResources) != 0;
    }

    bool ModeSnapshot::IsSessionTransition() const
    {
        return m_eExitReason == ModeExitReason::LoadGame || m_eExitReason == ModeExitReason::NewGame;
    }

    void CameraModeController::AttachInputTracker(InputTracker* inputTracker)
    {
        if (m_pInputTracker == inputTracker)
            return;
        if (m_pInputTracker)
            m_pInputTracker->SetCaptureMode(false);
        m_pInputTracker = inputTracker;
        SynchronizeInputCapture();
    }

    void CameraModeController::SynchronizeInputCapture()
    {
        if (m_pInputTracker)
            m_pInputTracker->SetCaptureMode(IsActive());
    }

    bool CameraModeController::BeginEnter()
    {
        if (m_eState != State::Inactive)
            return false;
        m_Snapshot.Reset();
        m_eState = State::Entering;
        SynchronizeInputCapture();
        return true;
    }

    void CameraModeController::MarkActive()
    {
        if (m_eState == State::Entering)
            m_eState = State::Active;
        SynchronizeInputCapture();
    }

    bool CameraModeController::BeginExit(ModeExitReason reason)
    {
        if (m_eState == State::Inactive)
            return false;

        // A real session transition or plugin shutdown takes precedence over a
        // previous user-requested exit that is still waiting for resource recovery.
        const bool terminalReason = reason == ModeExitReason::LoadGame ||
            reason == ModeExitReason::NewGame || reason == ModeExitReason::PluginShutdown;
        if ((m_eState != State::Exiting && m_eState != State::RecoveryRequired) || terminalReason)
            m_Snapshot.m_eExitReason = reason;
        else if (m_Snapshot.m_eExitReason == ModeExitReason::None)
            m_Snapshot.m_eExitReason = reason;

        m_eState = State::Exiting;
        SynchronizeInputCapture();
        return true;
    }

    void CameraModeController::CompleteExit(bool restorationComplete)
    {
        if (restorationComplete && !m_Snapshot.HasPendingRestores())
            m_eState = State::Inactive;
        else
            m_eState = State::RecoveryRequired;
        SynchronizeInputCapture();
    }

    void CameraModeController::RequireRecovery(ModeExitReason reason)
    {
        if (m_eState == State::Inactive)
            return;
        if (m_Snapshot.m_eExitReason == ModeExitReason::None)
            m_Snapshot.m_eExitReason = reason;
        m_eState = State::RecoveryRequired;
        SynchronizeInputCapture();
    }

    bool CameraModeController::IsActive() const
    {
        return m_eState == State::Entering || m_eState == State::Active;
    }

    bool CameraModeController::IsInactive() const
    {
        return m_eState == State::Inactive;
    }

    bool CameraModeController::NeedsRecovery() const
    {
        return m_eState == State::Exiting || m_eState == State::RecoveryRequired;
    }

    CameraModeController::State CameraModeController::GetState() const
    {
        return m_eState;
    }

    ModeSnapshot& CameraModeController::Snapshot()
    {
        return m_Snapshot;
    }

    const ModeSnapshot& CameraModeController::Snapshot() const
    {
        return m_Snapshot;
    }

    Settings MakeDefaultSettings()
    {
        Settings settings{};
        settings.m_bFreezeTime = true;
        settings.m_bHideHud = true;
        settings.m_bFreezeEffectsOnStart = true;
        settings.m_bShowHelpOnStart = true;
        settings.m_fMinFOV = 10.0f;
        settings.m_fMaxFOV = 150.0f;
        settings.m_fZoomSpeed = 30.0f;
        settings.m_fMouseSensitivity = 0.0026f;
        settings.m_fBaseMoveSpeed = 7.0f;
        settings.m_fFastMoveMultiplier = 3.0f;
        settings.m_fSlowMoveMultiplier = 0.22f;
        settings.m_nKeyModeToggle = KeyCode::V;
        settings.m_nKeyToggleTimePlayback = KeyCode::F3;
        settings.m_nKeyWideScreen = KeyCode::Tab;
        settings.m_nKeyZoomIn = KeyCode::LeftBracket;
        settings.m_nKeyZoomOut = KeyCode::RightBracket;
        settings.m_nKeyResetZoom = KeyCode::P;
        settings.m_nKeyShowHelp = KeyCode::F1;
        settings.m_nKeyFreezeEffects = KeyCode::F2;
        settings.m_nKeyCapturePhoto = KeyCode::F12;
        settings.m_nKeyAscend = KeyCode::Space;
        settings.m_nKeyDescend = KeyCode::C;
        settings.m_nKeyForward = KeyCode::W;
        settings.m_nKeyBackward = KeyCode::S;
        settings.m_nKeyLeft = KeyCode::A;
        settings.m_nKeyRight = KeyCode::D;
        settings.m_nKeyMoveFast = KeyCode::Shift;
        settings.m_nKeyMoveSlow = KeyCode::Control;
        settings.m_fHelpFontSize = 0.45f;
        settings.m_nHoursPerPress = 1;
        settings.m_nKeyHourDown = KeyCode::Minus;
        settings.m_nKeyHourUp = KeyCode::Equals;
        return settings;
    }

    const wchar_t* GetConfigurationPath(const wchar_t* iniFileName)
    {
        static wchar_t path[MAX_PATH] = {};
        static wchar_t cachedName[MAX_PATH] = {};
        const wchar_t* requestedName = (iniFileName && *iniFileName) ? iniFileName : L"AmazingSnapshot.ini";
        if (path[0] != L'\0' && std::wcscmp(cachedName, requestedName) == 0)
            return path;

        path[0] = L'\0';
        CopyWideString(cachedName, sizeof(cachedName) / sizeof(cachedName[0]), requestedName);

        HMODULE pluginModule = nullptr;
        const DWORD moduleFlags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
        if (!GetModuleHandleExW(moduleFlags,
            reinterpret_cast<LPCWSTR>(&GetConfigurationPath), &pluginModule))
        {
            CopyWideString(path, sizeof(path) / sizeof(path[0]), requestedName);
            return path;
        }

        wchar_t modulePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(pluginModule, modulePath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            CopyWideString(path, sizeof(path) / sizeof(path[0]), requestedName);
            return path;
        }

        wchar_t* lastBackslash = std::wcsrchr(modulePath, L'\\');
        wchar_t* lastSlash = std::wcsrchr(modulePath, L'/');
        if (!lastBackslash || (lastSlash && lastSlash > lastBackslash))
            lastBackslash = lastSlash;
        if (lastBackslash)
            lastBackslash[1] = L'\0';
        else
            modulePath[0] = L'\0';

        CopyWideString(path, sizeof(path) / sizeof(path[0]), modulePath);
        const std::size_t used = std::wcslen(path);
        const std::size_t capacity = sizeof(path) / sizeof(path[0]);
        if (used < capacity - 1)
            CopyWideString(path + used, capacity - used, requestedName);
        return path;
    }

    void LoadSettings(Settings& settings, const wchar_t* iniFileName)
    {
        const wchar_t* iniPath = GetConfigurationPath(iniFileName);
        settings.m_bFreezeTime = ReadIniBool(L"General", L"FreezeTime", true, iniPath);
        settings.m_bHideHud = ReadIniBool(L"General", L"HideHUD", true, iniPath);
        settings.m_bFreezeEffectsOnStart = ReadIniBool(L"General", L"FreezeEffectsOnStart", true, iniPath);
        settings.m_bShowHelpOnStart = ReadIniBool(L"General", L"ShowHelpOnStart", true, iniPath);
        settings.m_fHelpFontSize = ReadIniFloat(L"Interface", L"FontSize", 0.45f, iniPath);
        settings.m_fMinFOV = ReadIniFloat(L"Camera", L"MinimumFOV", 10.0f, iniPath);
        settings.m_fMaxFOV = ReadIniFloat(L"Camera", L"MaximumFOV", 150.0f, iniPath);
        settings.m_fZoomSpeed = ReadIniFloat(L"Camera", L"ZoomSpeed", -1.0f, iniPath);
        if (settings.m_fZoomSpeed < 0.0f)
        {
            // Backward compatibility: legacy ZoomStep was degrees per rendered frame.
            // Convert it using the historical 60 Hz reference into degrees per second.
            const float legacyZoomStep = ReadIniFloat(L"Camera", L"ZoomStep", -1.0f, iniPath);
            settings.m_fZoomSpeed = legacyZoomStep >= 0.0f ? legacyZoomStep * 60.0f : 30.0f;
        }
        settings.m_fMouseSensitivity = ReadIniFloat(L"Camera", L"MouseSensitivity", 0.0026f, iniPath);
        settings.m_fBaseMoveSpeed = ReadIniFloat(L"Camera", L"MoveSpeed", 7.0f, iniPath);
        settings.m_fFastMoveMultiplier = ReadIniFloat(L"Camera", L"FastMoveMultiplier", 3.0f, iniPath);
        settings.m_fSlowMoveMultiplier = ReadIniFloat(L"Camera", L"SlowMoveMultiplier", 0.22f, iniPath);
        settings.m_nHoursPerPress = static_cast<int>(ReadIniFloat(L"Environment", L"HoursPerPress", 1.0f, iniPath));

        settings.m_fHelpFontSize = ClampFloat(settings.m_fHelpFontSize, 0.18f, 0.75f);
        if (settings.m_fMinFOV < 1.0f || settings.m_fMinFOV > 60.0f) settings.m_fMinFOV = 10.0f;
        if (settings.m_fMaxFOV < 40.0f || settings.m_fMaxFOV > 179.0f) settings.m_fMaxFOV = 150.0f;
        if (settings.m_fMinFOV >= settings.m_fMaxFOV) { settings.m_fMinFOV = 10.0f; settings.m_fMaxFOV = 150.0f; }
        if (settings.m_fZoomSpeed < 1.0f || settings.m_fZoomSpeed > 300.0f) settings.m_fZoomSpeed = 30.0f;
        if (settings.m_fMouseSensitivity < 0.0001f || settings.m_fMouseSensitivity > 0.02f) settings.m_fMouseSensitivity = 0.0026f;
        if (settings.m_fBaseMoveSpeed < 0.1f || settings.m_fBaseMoveSpeed > 100.0f) settings.m_fBaseMoveSpeed = 7.0f;
        if (settings.m_fFastMoveMultiplier < 1.0f || settings.m_fFastMoveMultiplier > 20.0f) settings.m_fFastMoveMultiplier = 3.0f;
        if (settings.m_fSlowMoveMultiplier < 0.01f || settings.m_fSlowMoveMultiplier > 1.0f) settings.m_fSlowMoveMultiplier = 0.22f;
        if (settings.m_nHoursPerPress < 1 || settings.m_nHoursPerPress > 12) settings.m_nHoursPerPress = 1;

        settings.m_nKeyModeToggle = ReadIniKey(L"ModeToggle", L"SC_0x2F", KeyCode::V, iniPath);
        settings.m_nKeyToggleTimePlayback = ReadIniKey(L"ToggleTimePlayback", L"SC_0x3D", KeyCode::F3, iniPath);
        settings.m_nKeyWideScreen = ReadIniKey(L"Widescreen", L"SC_0x0F", KeyCode::Tab, iniPath);
        settings.m_nKeyZoomIn = ReadIniKey(L"ZoomIn", L"SC_0x1A", KeyCode::LeftBracket, iniPath);
        settings.m_nKeyZoomOut = ReadIniKey(L"ZoomOut", L"SC_0x1B", KeyCode::RightBracket, iniPath);
        settings.m_nKeyResetZoom = ReadIniKey(L"ResetZoom", L"SC_0x19", KeyCode::P, iniPath);
        settings.m_nKeyShowHelp = ReadIniKey(L"ToggleHelp", L"SC_0x3B", KeyCode::F1, iniPath);
        settings.m_nKeyFreezeEffects = ReadIniKey(L"ToggleEffectFreeze", L"SC_0x3C", KeyCode::F2, iniPath);
        settings.m_nKeyCapturePhoto = ReadIniKey(L"TakePhoto", L"SC_0x58", KeyCode::F12, iniPath);
        settings.m_nKeyAscend = ReadIniKey(L"Ascend", L"SC_0x39", KeyCode::Space, iniPath);
        settings.m_nKeyDescend = ReadIniKey(L"Descend", L"SC_0x2E", KeyCode::C, iniPath);
        settings.m_nKeyForward = ReadIniKey(L"MoveForward", L"SC_0x11", KeyCode::W, iniPath);
        settings.m_nKeyBackward = ReadIniKey(L"MoveBackward", L"SC_0x1F", KeyCode::S, iniPath);
        settings.m_nKeyLeft = ReadIniKey(L"MoveLeft", L"SC_0x1E", KeyCode::A, iniPath);
        settings.m_nKeyRight = ReadIniKey(L"MoveRight", L"SC_0x20", KeyCode::D, iniPath);
        settings.m_nKeyMoveFast = ReadIniKey(L"MoveFast", L"SC_0x200", KeyCode::Shift, iniPath);
        settings.m_nKeyMoveSlow = ReadIniKey(L"MoveSlow", L"SC_0x201", KeyCode::Control, iniPath);
        settings.m_nKeyHourDown = ReadIniKey(L"TimeHourDecrease", L"SC_0x0C", KeyCode::Minus, iniPath);
        settings.m_nKeyHourUp = ReadIniKey(L"TimeHourIncrease", L"SC_0x0D", KeyCode::Equals, iniPath);
    }

    std::string GetFileNameFromPath(const std::wstring& path)
    {
        const std::size_t separator = path.find_last_of(L"\\/");
        const std::wstring fileName = separator == std::wstring::npos ? path : path.substr(separator + 1);
        if (fileName.empty())
            return {};
        // GTA's CFont expects the game's narrow text encoding, not UTF-8. This
        // conversion is only for the on-screen status label; file I/O stays UTF-16.
        const int byteCount = WideCharToMultiByte(CP_ACP, 0, fileName.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (byteCount <= 1)
            return {};
        std::string gameText(static_cast<std::size_t>(byteCount), '\0');
        if (WideCharToMultiByte(CP_ACP, 0, fileName.c_str(), -1, gameText.data(), byteCount, nullptr, nullptr) == 0)
            return {};
        gameText.pop_back();
        return gameText;
    }

    std::string GetKeyLabel(int scanCode)
    {
        // These are physical US-layout labels for Set-1 scan codes. Do not use
        // GetKeyNameTextA here: it translates key names according to the current
        // Windows layout, which made the in-game help change after Alt+Shift.
        if (scanCode == KeyCode::Shift || scanCode == KeyCode::LeftShift || scanCode == KeyCode::RightShift) return "Shift";
        if (scanCode == KeyCode::Control || scanCode == KeyCode::LeftControl || scanCode == KeyCode::RightControl) return "Ctrl";
        if (scanCode == KeyCode::Alt || scanCode == KeyCode::LeftAlt || scanCode == KeyCode::RightAlt) return "Alt";
        switch (scanCode)
        {
        case KeyCode::Escape: return "Esc";
        case KeyCode::Digit1: return "1";
        case KeyCode::Digit2: return "2";
        case KeyCode::Digit3: return "3";
        case KeyCode::Digit4: return "4";
        case KeyCode::Digit5: return "5";
        case KeyCode::Digit6: return "6";
        case KeyCode::Digit7: return "7";
        case KeyCode::Digit8: return "8";
        case KeyCode::Digit9: return "9";
        case KeyCode::Digit0: return "0";
        case KeyCode::Minus: return "-";
        case KeyCode::Equals: return "=";
        case KeyCode::Backspace: return "Backspace";
        case KeyCode::Tab: return "Tab";
        case KeyCode::Q: return "Q";
        case KeyCode::W: return "W";
        case KeyCode::E: return "E";
        case KeyCode::R: return "R";
        case KeyCode::T: return "T";
        case KeyCode::Y: return "Y";
        case KeyCode::U: return "U";
        case KeyCode::I: return "I";
        case KeyCode::O: return "O";
        case KeyCode::P: return "P";
        case KeyCode::LeftBracket: return "[";
        case KeyCode::RightBracket: return "]";
        case KeyCode::Enter: return "Enter";
        case KeyCode::A: return "A";
        case KeyCode::S: return "S";
        case KeyCode::D: return "D";
        case KeyCode::F: return "F";
        case KeyCode::G: return "G";
        case KeyCode::H: return "H";
        case KeyCode::J: return "J";
        case KeyCode::K: return "K";
        case KeyCode::L: return "L";
        case KeyCode::Semicolon: return ";";
        case KeyCode::Apostrophe: return "'";
        case KeyCode::Grave: return "`";
        case KeyCode::Backslash: return "\\";
        case KeyCode::Z: return "Z";
        case KeyCode::X: return "X";
        case KeyCode::C: return "C";
        case KeyCode::V: return "V";
        case KeyCode::B: return "B";
        case KeyCode::N: return "N";
        case KeyCode::M: return "M";
        case KeyCode::Comma: return ",";
        case KeyCode::Period: return ".";
        case KeyCode::Slash: return "/";
        case KeyCode::NumMultiply: return "Num*";
        case KeyCode::Space: return "Space";
        case KeyCode::CapsLock: return "CapsLock";
        case KeyCode::F1: return "F1";
        case KeyCode::F2: return "F2";
        case KeyCode::F3: return "F3";
        case KeyCode::F4: return "F4";
        case KeyCode::F5: return "F5";
        case KeyCode::F6: return "F6";
        case KeyCode::F7: return "F7";
        case KeyCode::F8: return "F8";
        case KeyCode::F9: return "F9";
        case KeyCode::F10: return "F10";
        case KeyCode::NumLock: return "NumLock";
        case KeyCode::ScrollLock: return "ScrollLock";
        case KeyCode::Num7: return "Num7";
        case KeyCode::Num8: return "Num8";
        case KeyCode::Num9: return "Num9";
        case KeyCode::NumMinus: return "Num-";
        case KeyCode::Num4: return "Num4";
        case KeyCode::Num5: return "Num5";
        case KeyCode::Num6: return "Num6";
        case KeyCode::NumPlus: return "Num+";
        case KeyCode::Num1: return "Num1";
        case KeyCode::Num2: return "Num2";
        case KeyCode::Num3: return "Num3";
        case KeyCode::Num0: return "Num0";
        case KeyCode::NumDecimal: return "Num.";
        case KeyCode::IsoLessGreater: return "\\|";
        case KeyCode::F11: return "F11";
        case KeyCode::F12: return "F12";
        case KeyCode::NumEnter: return "NumEnter";
        case KeyCode::NumDivide: return "Num/";
        case KeyCode::Home: return "Home";
        case KeyCode::Up: return "Up";
        case KeyCode::PageUp: return "PgUp";
        case KeyCode::Left: return "Left";
        case KeyCode::Right: return "Right";
        case KeyCode::End: return "End";
        case KeyCode::Down: return "Down";
        case KeyCode::PageDown: return "PgDn";
        case KeyCode::Insert: return "Insert";
        case KeyCode::Delete: return "Delete";
        default: break;
        }

        char label[24] = {};
        std::snprintf(label, sizeof(label), "SC_0x%03X", scanCode);
        return label;
    }

    std::array<std::string, 6> BuildCommonCameraHelpLines(const Settings& settings)
    {
        return {{
            "Alt+" + GetKeyLabel(settings.m_nKeyModeToggle) + ": toggle mode | " +
                GetKeyLabel(settings.m_nKeyModeToggle) + ": FREE/FOLLOW | " +
                GetKeyLabel(settings.m_nKeyToggleTimePlayback) + ": play/pause time",
            "Esc: open/close game menu | " + GetKeyLabel(settings.m_nKeyForward) + "/" +
                GetKeyLabel(settings.m_nKeyLeft) + "/" + GetKeyLabel(settings.m_nKeyBackward) + "/" +
                GetKeyLabel(settings.m_nKeyRight) + " / arrows: move",
            GetKeyLabel(settings.m_nKeyAscend) + " / PgUp: rise    " + GetKeyLabel(settings.m_nKeyDescend) + " / PgDn: lower",
            "Mouse: look    LMB: lock X    RMB: lock Y",
            GetKeyLabel(settings.m_nKeyMoveFast) + ": faster    " + GetKeyLabel(settings.m_nKeyMoveSlow) + ": slower",
            "Zoom: hold " + GetKeyLabel(settings.m_nKeyZoomIn) + "/" +
                GetKeyLabel(settings.m_nKeyZoomOut) + " or Num* in / Num/ out | " +
                GetKeyLabel(settings.m_nKeyResetZoom) + ": reset zoom"
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

    float ClampFloat(float value, float minimum, float maximum)
    {
        return std::max(minimum, std::min(maximum, value));
    }

    float UpdateContinuousCameraFOV(float currentFOV, float initialFOV, const Settings& settings,
        bool zoomInDown, bool zoomOutDown, bool resetPressed, float deltaTime)
    {
        if (resetPressed)
            currentFOV = initialFOV;
        else
        {
            // Use wall-clock delta because the game simulation may be paused while
            // drawing callbacks continue. A 250 ms cap prevents jumps after stalls.
            deltaTime = ClampFloat(deltaTime, 0.0f, 0.25f);
            const float fovDelta = settings.m_fZoomSpeed * deltaTime;
            if (zoomInDown && !zoomOutDown)
                currentFOV -= fovDelta;
            else if (zoomOutDown && !zoomInDown)
                currentFOV += fovDelta;
        }
        return ClampFloat(currentFOV, settings.m_fMinFOV, settings.m_fMaxFOV);
    }

    float UpdateFrameStepCameraFOV(float currentFOV, float initialFOV, const Settings& settings,
        bool zoomInDown, bool zoomOutDown, bool resetPressed)
    {
        if (resetPressed)
            currentFOV = initialFOV;
        else
        {
            // Same observable stepping as the working pre-refactor III/VC code.
            // ZoomSpeed is stored in degrees/second, so dividing by the legacy 60 Hz
            // reference reproduces ZoomStep (e.g. 30 -> 0.5 degrees per update).
            const float fovStep = settings.m_fZoomSpeed / 60.0f;
            if (zoomInDown && !zoomOutDown)
                currentFOV -= fovStep;
            else if (zoomOutDown && !zoomInDown)
                currentFOV += fovStep;
        }
        return ClampFloat(currentFOV, settings.m_fMinFOV, settings.m_fMaxFOV);
    }

    bool InputTracker::Initialize()
    {
        const bool foreground = IsCurrentProcessForeground();
        s_bInputProcessForeground.store(foreground, std::memory_order_relaxed);
        if (!foreground)
        {
            ClearPhysicalKeyStates();
            m_abPreviousKeys.fill(false);
            s_bAltChordSuppressionActive.store(false, std::memory_order_relaxed);
            s_bGameMenuInputActive.store(false, std::memory_order_relaxed);
            s_bNativeGameMenuObservedActive.store(false, std::memory_order_relaxed);
            s_bEscapeKeyUpWasForwarded.store(false, std::memory_order_relaxed);
        }

        // A global low-level hook is only needed while the plugin captures game input
        // or briefly while Alt is held for the Alt+V mode-toggle chord. It is not
        // installed during ordinary gameplay/loading, avoiding a permanent system-wide
        // keyboard hook and its impact on the Windows input path.
        const bool shouldHook = foreground &&
            (s_bKeyboardCaptureActive.load(std::memory_order_relaxed) || IsAnyAltDownNow());
        if (shouldHook)
            return EnsureKeyboardHookInstalled();

        if (s_pKeyboardHook)
        {
            UnhookWindowsHookEx(s_pKeyboardHook);
            s_pKeyboardHook = nullptr;
            s_nNextKeyboardHookAttemptTick = 0;
            ClearPhysicalKeyStates();
            s_bAltChordSuppressionActive.store(false, std::memory_order_relaxed);
        }
        return false;
    }

    void InputTracker::Shutdown()
    {
        s_bKeyboardCaptureActive.store(false, std::memory_order_relaxed);
        s_bGameMenuInputActive.store(false, std::memory_order_relaxed);
        s_bNativeGameMenuObservedActive.store(false, std::memory_order_relaxed);
        s_bEscapeKeyUpWasForwarded.store(false, std::memory_order_relaxed);
        if (s_pKeyboardHook)
        {
            UnhookWindowsHookEx(s_pKeyboardHook);
            s_pKeyboardHook = nullptr;
        }
        s_nNextKeyboardHookAttemptTick = 0;
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

    namespace
    {
        bool IsAsyncScanCodeDownForZoom(int scanCode)
        {
            if (scanCode < 0 || scanCode >= KeyCode::ScanCodeCount)
                return false;

            UINT windowsScanCode = static_cast<UINT>(scanCode & 0xFF);
            if ((scanCode & 0x100) != 0)
                windowsScanCode |= 0xE000;

            const HKL layout = GetKeyboardLayout(0);
            const UINT virtualKey = MapVirtualKeyExW(windowsScanCode, MAPVK_VSC_TO_VK_EX, layout);
            return virtualKey != 0 && (GetAsyncKeyState(static_cast<int>(virtualKey)) & 0x8000) != 0;
        }
    }

    bool IsCameraZoomInDown(const InputTracker& input, const Settings& settings)
    {
        // Numpad +/- must never be interpreted as FOV controls, even if an old INI
        // assigned one of them to ZoomIn. They are reserved for game-hour changes.
        const bool configuredKeyAllowed = settings.m_nKeyZoomIn != KeyCode::NumPlus &&
            settings.m_nKeyZoomIn != KeyCode::NumMinus &&
            settings.m_nKeyZoomIn != KeyCode::NumMultiply &&
            settings.m_nKeyZoomIn != KeyCode::NumDivide;

        return (configuredKeyAllowed &&
                (input.IsKeyDown(settings.m_nKeyZoomIn) || IsAsyncScanCodeDownForZoom(settings.m_nKeyZoomIn))) ||
            input.IsKeyDown(KeyCode::LeftBracket) || IsAsyncScanCodeDownForZoom(KeyCode::LeftBracket) ||
            input.IsKeyDown(KeyCode::NumMultiply) || IsAsyncScanCodeDownForZoom(KeyCode::NumMultiply) ||
            (GetAsyncKeyState(VK_OEM_4) & 0x8000) != 0;
    }

    bool IsCameraZoomOutDown(const InputTracker& input, const Settings& settings)
    {
        const bool configuredKeyAllowed = settings.m_nKeyZoomOut != KeyCode::NumPlus &&
            settings.m_nKeyZoomOut != KeyCode::NumMinus &&
            settings.m_nKeyZoomOut != KeyCode::NumMultiply &&
            settings.m_nKeyZoomOut != KeyCode::NumDivide;

        return (configuredKeyAllowed &&
                (input.IsKeyDown(settings.m_nKeyZoomOut) || IsAsyncScanCodeDownForZoom(settings.m_nKeyZoomOut))) ||
            input.IsKeyDown(KeyCode::RightBracket) || IsAsyncScanCodeDownForZoom(KeyCode::RightBracket) ||
            input.IsKeyDown(KeyCode::NumDivide) || IsAsyncScanCodeDownForZoom(KeyCode::NumDivide) ||
            (GetAsyncKeyState(VK_OEM_6) & 0x8000) != 0;
    }

    bool InputTracker::WasKeyPressed(int scanCode)
    {
        if (scanCode < 0 || scanCode >= static_cast<int>(m_abPreviousKeys.size()))
            return false;
        const bool down = IsKeyDown(scanCode);
        const bool pressed = down && !m_abPreviousKeys[static_cast<std::size_t>(scanCode)];
        m_abPreviousKeys[static_cast<std::size_t>(scanCode)] = down;
        return pressed;
    }

    bool WasDigitPressed(InputTracker& input, int digit)
    {
        if (digit < 0 || digit > 9)
            return false;

        const int numberRowKey = KeyCode::NumberRowKey(digit);
        const int numpadKey = KeyCode::NumpadKey(digit);
        const bool numberRowPressed = input.WasKeyPressed(numberRowKey);
        const bool numpadPressed = input.WasKeyPressed(numpadKey);
        return numberRowPressed || numpadPressed;
    }

    int CalculateWrappedGameHour(int currentHour, int direction, int hoursPerPress)
    {
        int hour = (currentHour + direction * hoursPerPress) % 24;
        if (hour < 0)
            hour += 24;
        return hour;
    }

    void ReassertGameTimeScale(CameraModeController& controller, const InputTracker& input,
        bool timePlaybackEnabled, float& gameTimeScale)
    {
        if (!controller.IsActive() || input.IsGameMenuInputActive())
            return;

        // Playback selection is authoritative. Snapshot capture/restoration and the concrete
        // game timer remain controlled by the adapter, while this policy is common to all games.
        const float desiredTimeScale = timePlaybackEnabled
            ? controller.Snapshot().m_fSavedTimeScale
            : 0.0f;
        if (gameTimeScale != desiredTimeScale)
        {
            gameTimeScale = desiredTimeScale;
            controller.Snapshot().MarkApplied(ModeResource::TimeScale);
        }
    }

    void ToggleGameTimePlayback(CameraModeController& controller, const InputTracker& input,
        bool& timePlaybackEnabled, float& gameTimeScale)
    {
        if (!controller.IsActive() || input.IsGameMenuInputActive())
            return;

        timePlaybackEnabled = !timePlaybackEnabled;
        // Apply immediately after the toggle, including while a camera handoff is pending.
        ReassertGameTimeScale(controller, input, timePlaybackEnabled, gameTimeScale);
    }

    bool InputTracker::IsMouseButtonDown(int virtualKey) const
    {
        if (virtualKey != VK_LBUTTON && virtualKey != VK_RBUTTON && virtualKey != VK_MBUTTON)
            return false;
        return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    }

    void InputTracker::SetCaptureMode(bool active)
    {
        s_bKeyboardCaptureActive.store(active, std::memory_order_relaxed);
        if (active && s_bInputProcessForeground.load(std::memory_order_relaxed))
            EnsureKeyboardHookInstalled();
        if (!active)
        {
            s_bGameMenuInputActive.store(false, std::memory_order_relaxed);
            s_bNativeGameMenuObservedActive.store(false, std::memory_order_relaxed);
            s_bEscapeKeyUpWasForwarded.store(false, std::memory_order_relaxed);
        }
        // When leaving capture mode, Initialize() will remove the hook as soon as
        // Alt is released, preserving the key-up edge of the Alt+V chord.
    }

    void InputTracker::SetModeToggleKey(int scanCode)
    {
        if (scanCode < 0 || scanCode >= KeyCode::ScanCodeCount)
            scanCode = -1;
        s_nAltChordKeyScanCode.store(scanCode, std::memory_order_relaxed);
        s_bAltChordSuppressionActive.store(false, std::memory_order_relaxed);
    }

    bool InputTracker::IsGameMenuInputActive() const
    {
        return s_bGameMenuInputActive.load(std::memory_order_relaxed);
    }

    void InputTracker::SetGameMenuInputActive(bool active)
    {
        if (active)
        {
            s_bNativeGameMenuObservedActive.store(true, std::memory_order_relaxed);
            s_bGameMenuInputActive.store(true, std::memory_order_relaxed);
        }
        else if (s_bNativeGameMenuObservedActive.exchange(false, std::memory_order_relaxed))
        {
            // The menu really was active and the native game now reports it closed
            // (for example, Resume selected with Enter instead of a second Esc).
            s_bGameMenuInputActive.store(false, std::memory_order_relaxed);
            s_bEscapeKeyUpWasForwarded.store(false, std::memory_order_relaxed);
            // Drop keys held while the pause menu was active so they cannot remain
            // stuck or suppress fresh edges after Resume. The hook records new edges.
            ClearPhysicalKeyStates();
            s_bAltChordSuppressionActive.store(false, std::memory_order_relaxed);
            m_abPreviousKeys.fill(false);
        }
    }

    void InputTracker::Reset()
    {
        m_abPreviousKeys.fill(false);
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
