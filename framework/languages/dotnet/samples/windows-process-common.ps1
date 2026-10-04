if ($IsWindows -and -not ("Zlink.SampleWindowsProcessGroup" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace Zlink
{
    public static class SampleWindowsProcessGroup
    {
        private const uint CreateNewProcessGroup = 0x00000200;
        private const uint CtrlBreakEvent = 1;
        private const uint GenericWrite = 0x40000000;
        private const uint FileShareRead = 0x00000001;
        private const uint FileShareWrite = 0x00000002;
        private const uint FileShareDelete = 0x00000004;
        private const uint CreateAlways = 2;
        private const uint FileAttributeNormal = 0x00000080;
        private const uint StartfUseStdHandles = 0x00000100;
        private const int StdInputHandle = -10;
        private static readonly IntPtr InvalidHandleValue = new IntPtr(-1);

        [StructLayout(LayoutKind.Sequential)]
        private struct SecurityAttributes
        {
            public int Length;
            public IntPtr SecurityDescriptor;
            [MarshalAs(UnmanagedType.Bool)] public bool InheritHandle;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct StartupInfo
        {
            public int Size;
            public string Reserved;
            public string Desktop;
            public string Title;
            public int X;
            public int Y;
            public int XSize;
            public int YSize;
            public int XCountChars;
            public int YCountChars;
            public int FillAttribute;
            public int Flags;
            public short ShowWindow;
            public short Reserved2Size;
            public IntPtr Reserved2;
            public IntPtr StandardInput;
            public IntPtr StandardOutput;
            public IntPtr StandardError;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct ProcessInformation
        {
            public IntPtr Process;
            public IntPtr Thread;
            public int ProcessId;
            public int ThreadId;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CreateProcess(
            string applicationName,
            StringBuilder commandLine,
            IntPtr processAttributes,
            IntPtr threadAttributes,
            [MarshalAs(UnmanagedType.Bool)] bool inheritHandles,
            uint creationFlags,
            IntPtr environment,
            string currentDirectory,
            ref StartupInfo startupInfo,
            out ProcessInformation processInformation);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateFile(
            string fileName,
            uint desiredAccess,
            uint shareMode,
            ref SecurityAttributes securityAttributes,
            uint creationDisposition,
            uint flagsAndAttributes,
            IntPtr templateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr handle);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr GetStdHandle(int standardHandle);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GenerateConsoleCtrlEvent(uint controlEvent, uint processGroupId);

        public static Process Start(
            string filePath,
            string[] arguments,
            string workingDirectory,
            string standardOutputPath,
            string standardErrorPath)
        {
            SecurityAttributes security = new SecurityAttributes
            {
                Length = Marshal.SizeOf(typeof(SecurityAttributes)),
                InheritHandle = true
            };
            IntPtr standardOutput = OpenLog(standardOutputPath, ref security);
            IntPtr standardError = IntPtr.Zero;
            ProcessInformation processInformation = new ProcessInformation();
            try
            {
                standardError = OpenLog(standardErrorPath, ref security);
                StartupInfo startupInfo = new StartupInfo
                {
                    Size = Marshal.SizeOf(typeof(StartupInfo)),
                    Flags = (int)StartfUseStdHandles,
                    StandardInput = GetStdHandle(StdInputHandle),
                    StandardOutput = standardOutput,
                    StandardError = standardError
                };
                string applicationName = filePath;
                StringBuilder commandLine;
                string extension = System.IO.Path.GetExtension(filePath);
                if (String.Equals(extension, ".cmd", StringComparison.OrdinalIgnoreCase) ||
                    String.Equals(extension, ".bat", StringComparison.OrdinalIgnoreCase))
                {
                    string commandInterpreter = Environment.GetEnvironmentVariable("ComSpec");
                    if (String.IsNullOrWhiteSpace(commandInterpreter))
                        throw new InvalidOperationException("ComSpec is required to launch a Windows batch command.");
                    applicationName = commandInterpreter;
                    commandLine = new StringBuilder(Quote(commandInterpreter)).Append(" /d /s /c \"")
                        .Append(BuildCommandLine(filePath, arguments)).Append('"');
                }
                else
                {
                    commandLine = BuildCommandLine(filePath, arguments);
                }

                if (!CreateProcess(
                    applicationName,
                    commandLine,
                    IntPtr.Zero,
                    IntPtr.Zero,
                    true,
                    CreateNewProcessGroup,
                    IntPtr.Zero,
                    String.IsNullOrWhiteSpace(workingDirectory) ? null : workingDirectory,
                    ref startupInfo,
                    out processInformation))
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error(),
                        "Failed to start the sample process group.");
                }

                Process process = Process.GetProcessById(processInformation.ProcessId);
                process.Refresh();
                return process;
            }
            finally
            {
                if (processInformation.Thread != IntPtr.Zero) CloseHandle(processInformation.Thread);
                if (processInformation.Process != IntPtr.Zero) CloseHandle(processInformation.Process);
                if (standardError != IntPtr.Zero && standardError != InvalidHandleValue) CloseHandle(standardError);
                if (standardOutput != IntPtr.Zero && standardOutput != InvalidHandleValue) CloseHandle(standardOutput);
            }
        }

        public static void SendBreak(int processGroupId)
        {
            if (!GenerateConsoleCtrlEvent(CtrlBreakEvent, unchecked((uint)processGroupId)))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "Failed to send CTRL_BREAK_EVENT to sample process group " + processGroupId + ".");
            }
        }

        private static IntPtr OpenLog(string path, ref SecurityAttributes security)
        {
            IntPtr handle = CreateFile(
                path,
                GenericWrite,
                FileShareRead | FileShareWrite | FileShareDelete,
                ref security,
                CreateAlways,
                FileAttributeNormal,
                IntPtr.Zero);
            if (handle == InvalidHandleValue)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Failed to open sample log " + path + ".");
            }
            return handle;
        }

        private static StringBuilder BuildCommandLine(string filePath, string[] arguments)
        {
            StringBuilder commandLine = new StringBuilder(Quote(filePath));
            foreach (string argument in arguments)
            {
                commandLine.Append(' ').Append(Quote(argument));
            }
            return commandLine;
        }

        private static string Quote(string argument)
        {
            if (argument.Length > 0 && argument.IndexOfAny(new[] { ' ', '\t', '\n', '\v', '"' }) < 0)
                return argument;

            StringBuilder quoted = new StringBuilder("\"");
            int backslashes = 0;
            foreach (char character in argument)
            {
                if (character == '\\')
                {
                    backslashes++;
                    continue;
                }
                if (character == '"')
                {
                    quoted.Append('\\', backslashes * 2 + 1).Append(character);
                    backslashes = 0;
                    continue;
                }
                quoted.Append('\\', backslashes).Append(character);
                backslashes = 0;
            }
            return quoted.Append('\\', backslashes * 2).Append('"').ToString();
        }
    }
}
'@
}
