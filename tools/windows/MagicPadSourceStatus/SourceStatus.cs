using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
namespace MagicPadSourceStatus {
    [StructLayout(LayoutKind.Sequential)]
    public struct Status {
        public uint Size, Version;
        public int Connected, ModeEnabled, LastStatus;
        public int Packets, TouchPackets, InvalidPackets, Reports, Reconnects;
        public uint InputMode, SurfaceEnabled, ButtonEnabled;
        public int TransportStage, FailureStage, FailureBrbStatus, FailureBtStatus, FailureBrbType;
        public int ControlOpens, InterruptOpens, FeatureWrites, HandshakeCode;
        public int BatteryValid, BatteryPercent, BatteryFlags, BatteryAgeSeconds;
        public int BatteryReports, BatteryQueries, BatteryLastStatus;
        public int BatteryWriteStatus, BatteryReadStatus, BatteryReadLength, BatteryReadRemaining;
        public uint BatteryResponse;
        public int BatteryPropertyStatus, BatteryPropertyPercent, BatteryPropertyUpdates;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct InterfaceData { public uint Size; public Guid ClassGuid; public uint Flags; public UIntPtr Reserved; }
    public static class Reader {
        [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern IntPtr SetupDiGetClassDevsW(ref Guid guid, IntPtr enumerator, IntPtr window, uint flags);
        [DllImport("setupapi.dll", SetLastError=true)]
        static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr device, ref Guid guid, uint index, ref InterfaceData data);
        [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern bool SetupDiGetDeviceInterfaceDetailW(IntPtr set, ref InterfaceData data, IntPtr detail, uint size, out uint required, IntPtr device);
        [DllImport("setupapi.dll")]
        static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError=true)]
        static extern bool DeviceIoControl(SafeFileHandle handle, uint code, IntPtr input, uint inputSize, out Status status, uint outputSize, out uint returned, IntPtr overlapped);
        public static Status[] Read() {
            var guid = new Guid("84f01477-84a8-46d6-898b-73a4907edd46");
            var results = new List<Status>();
            var set = SetupDiGetClassDevsW(ref guid, IntPtr.Zero, IntPtr.Zero, 0x12);
            if (set == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
            try {
                for (uint index = 0; ; ++index) {
                    var data = new InterfaceData { Size = (uint)Marshal.SizeOf(typeof(InterfaceData)) };
                    if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref guid, index, ref data)) {
                        int error = Marshal.GetLastWin32Error();
                        if (error == 259) break;
                        throw new Win32Exception(error);
                    }
                    uint required;
                    SetupDiGetDeviceInterfaceDetailW(set, ref data, IntPtr.Zero, 0, out required, IntPtr.Zero);
                    if (required < 8 || required > 65536) throw new InvalidOperationException("Invalid interface detail size");
                    IntPtr detail = Marshal.AllocHGlobal((int)required);
                    try {
                        Marshal.WriteInt32(detail, 8);
                        if (!SetupDiGetDeviceInterfaceDetailW(set, ref data, detail, required, out required, IntPtr.Zero))
                            throw new Win32Exception(Marshal.GetLastWin32Error());
                        string path = Marshal.PtrToStringUni(IntPtr.Add(detail, 4));
                        using (var file = CreateFileW(path, 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
                            if (file.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
                            Status status; uint returned;
                            uint size = (uint)Marshal.SizeOf(typeof(Status));
                            if (!DeviceIoControl(file, (0x22u << 16) | (1u << 14) | (0x801u << 2),
                                IntPtr.Zero, 0, out status, size, out returned, IntPtr.Zero))
                                throw new Win32Exception(Marshal.GetLastWin32Error());
                            if (returned != status.Size ||
                                !((status.Version == 1 && returned == 52) || (status.Version == 2 && returned == 88) ||
                                  (status.Version == 3 && returned == 116) || (status.Version == 4 && returned == 136) ||
                                  (status.Version == 5 && returned == 148)))
                                throw new InvalidOperationException("Unexpected source status schema");
                            if (status.Version < 3) {
                                status.BatteryValid = 0; status.BatteryPercent = -1;
                                status.BatteryFlags = -1; status.BatteryAgeSeconds = -1;
                            }
                            if (status.Version < 5) status.BatteryPropertyPercent = -1;
                            results.Add(status);
                        }
                    } finally { Marshal.FreeHGlobal(detail); }
                }
            } finally { SetupDiDestroyDeviceInfoList(set); }
            return results.ToArray();
        }
    }
}
