/*
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Solstice.exe in the root of the Windows ZIP package: starts
 * bin\solstice.exe with the same arguments and exits. A shortcut (.lnk)
 * stores an absolute target, which breaks once the ZIP is extracted
 * elsewhere; this program finds the application next to itself.
 * Installed as bin\solstice-launcher.exe and moved to the package root by
 * packaging/windows/package-complete.py (docs/agent/executable-name.md).
 */

#include <windows.h>

#include <string>

namespace
{
/// The folder holding this program, with a trailing backslash
std::wstring ownFolder()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (length == 0) {
            return std::wstring();
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    return path.substr(0, path.find_last_of(L'\\') + 1);
}

/// The command line after the program name, as Windows passed it
const wchar_t *argumentsAfterProgram()
{
    const wchar_t *p = GetCommandLineW();
    if (*p == L'"') {
        p++;
        while (*p && *p != L'"') {
            p++;
        }
        if (*p == L'"') {
            p++;
        }
    } else {
        while (*p && *p != L' ' && *p != L'\t') {
            p++;
        }
    }
    while (*p == L' ' || *p == L'\t') {
        p++;
    }
    return p;
}

bool fileExists(const std::wstring &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    const std::wstring folder = ownFolder();
    // the package root holds bin\solstice.exe; as installed, the launcher
    // sits next to solstice.exe
    std::wstring application = folder + L"bin\\solstice.exe";
    if (!fileExists(application)) {
        application = folder + L"solstice.exe";
    }

    std::wstring commandLine = L"\"" + application + L"\"";
    const wchar_t *arguments = argumentsAfterProgram();
    if (*arguments) {
        commandLine += L" ";
        commandLine += arguments;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(application.c_str(),
                        commandLine.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        0,
                        nullptr,
                        nullptr,
                        &startup,
                        &process)) {
        const std::wstring message = L"Solstice could not be started:\n" + application;
        MessageBoxW(nullptr, message.c_str(), L"Solstice", MB_OK | MB_ICONERROR);
        return 1;
    }
    // let the application's window come to the front
    AllowSetForegroundWindow(process.dwProcessId);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
