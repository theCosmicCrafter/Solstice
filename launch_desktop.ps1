$env:TEMP = "$env:LOCALAPPDATA\Temp"
$env:TMP = "$env:LOCALAPPDATA\Temp"

Add-Type @"
using System;
using System.Runtime.InteropServices;

public class DesktopLauncher {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct STARTUPINFO {
        public int cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public int dwX;
        public int dwY;
        public int dwXSize;
        public int dwYSize;
        public int dwXCountChars;
        public int dwYCountChars;
        public int dwFillAttribute;
        public int dwFlags;
        public short wShowWindow;
        public short cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput;
        public IntPtr hStdOutput;
        public IntPtr hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PROCESS_INFORMATION {
        public IntPtr hProcess;
        public IntPtr hThread;
        public int dwProcessId;
        public int dwThreadId;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool CreateProcess(
        string lpApplicationName,
        string lpCommandLine,
        IntPtr lpProcessAttributes,
        IntPtr lpThreadAttributes,
        bool bInheritHandles,
        uint dwCreationFlags,
        IntPtr lpEnvironment,
        string lpCurrentDirectory,
        ref STARTUPINFO lpStartupInfo,
        out PROCESS_INFORMATION lpProcessInformation);

    public static int LaunchOnDefaultDesktop(string exePath, string workDir) {
        STARTUPINFO si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(si);
        si.lpDesktop = @"winsta0\default";

        PROCESS_INFORMATION pi = new PROCESS_INFORMATION();
        string cmdLine = "\"" + exePath + "\"";
        bool success = CreateProcess(
            null,
            cmdLine,
            IntPtr.Zero,
            IntPtr.Zero,
            false,
            0,
            IntPtr.Zero,
            workDir,
            ref si,
            out pi);

        if (!success) {
            int err = Marshal.GetLastWin32Error();
            Console.WriteLine("CreateProcess failed with error: " + err);
            return -1;
        }

        Console.WriteLine("Launched successfully! PID: " + pi.dwProcessId);
        return pi.dwProcessId;
    }
}
"@

$exe = "F:\solstice-test\app-49697aad\Solstice-0.5.0-windows-x64-49697aadce6c\bin\solstice.exe"
$dir = "F:\solstice-test\app-49697aad\Solstice-0.5.0-windows-x64-49697aadce6c\bin"
if (-not (Test-Path $exe)) {
    $exe = "F:\solstice-test\app-a50aa6f7\Solstice-0.2.0-beta.4-windows-x64-a50aa6f7ca63\bin\solstice.exe"
    $dir = "F:\solstice-test\app-a50aa6f7\Solstice-0.2.0-beta.4-windows-x64-a50aa6f7ca63\bin"
}
[DesktopLauncher]::LaunchOnDefaultDesktop($exe, $dir)
