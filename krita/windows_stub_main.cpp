/*
 * SPDX-FileCopyrightText: 2021 Alvin Wong <alvin@alvinhc.com>
 * SPDX-FileCopyrightText: 2026 Solstice contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {
    __declspec(dllimport) int krita_main(int argc, char **argv);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    // If started without an existing console (e.g. double-clicked in Explorer,
    // launched from a dock, or opened via shortcut), detach the allocated console
    // window so a blank console window labeled solstice.com does not appear.
    DWORD consoleProcesses[2];
    if (GetConsoleProcessList(consoleProcesses, 2) <= 1) {
        FreeConsole();
    }
#endif
    return krita_main(argc, argv);
}
