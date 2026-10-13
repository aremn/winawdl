// SPDX-License-Identifier: MIT
using System.IO;
using Microsoft.Win32;
using System.Runtime.InteropServices;

namespace Winawdl.MediaTek;

public sealed record DeviceSnapshot(string InstanceId, string HardwareId, string DriverVersion, string DriverPath,
                                    Guid InterfaceId, int InterfaceState)
{
    public static DeviceSnapshot Read()
    {
        // The present PCI devnode points to its current class key. This avoids
        // stale adapter entries and does not depend on a fixed oemNN.inf name.
        using var pci = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Enum\PCI")
            ?? throw new InvalidOperationException("PCI device registry is unavailable.");
        foreach (var deviceName in pci.GetSubKeyNames().Where(name => name.StartsWith("VEN_14C3&DEV_7925", StringComparison.OrdinalIgnoreCase)))
        {
            using var device = pci.OpenSubKey(deviceName)!;
            foreach (var instanceName in device.GetSubKeyNames())
            {
                using var instance = device.OpenSubKey(instanceName)!;
                if (instance.GetValue("Service") as string != "mtkwecx") continue;
                if (instance.GetValue("Driver") is not string driverKey) continue;
                using var config = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\" + driverKey);
                if (config?.GetValue("NetCfgInstanceId") is not string interfaceText || !Guid.TryParse(interfaceText, out var id)) continue;
                var state = WlanState(id);
                if (state < 0) continue;
                return new DeviceSnapshot(@"PCI\" + deviceName + @"\" + instanceName,
                    @"PCI\" + deviceName, config.GetValue("DriverVersion") as string ?? "",
                    Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "System32", "drivers", "mtkwecx.sys"), id, state);
            }
        }
        throw new InvalidOperationException("No present MT7925 Wi-Fi interface was found.");
    }

    private static int WlanState(Guid wanted)
    {
        var error = WlanOpenHandle(2, IntPtr.Zero, out _, out var client);
        if (error != 0) throw new System.ComponentModel.Win32Exception((int)error, "WLAN service");
        try
        {
            error = WlanEnumInterfaces(client, IntPtr.Zero, out var list);
            if (error != 0) throw new System.ComponentModel.Win32Exception((int)error, "WLAN interfaces");
            try
            {
                var count = Marshal.ReadInt32(list);
                if (count < 0 || count > 128) throw new InvalidDataException("Invalid WLAN interface list.");
                for (var index = 0; index < count; index++)
                {
                    var entry = IntPtr.Add(list, 8 + index * 532);
                    if (Marshal.PtrToStructure<Guid>(entry) == wanted) return Marshal.ReadInt32(entry, 528);
                }
                return -1;
            }
            finally { WlanFreeMemory(list); }
        }
        finally { WlanCloseHandle(client, IntPtr.Zero); }
    }

    [DllImport("wlanapi.dll")]
    private static extern uint WlanOpenHandle(uint version, IntPtr reserved, out uint negotiated, out IntPtr client);
    [DllImport("wlanapi.dll")]
    private static extern uint WlanEnumInterfaces(IntPtr client, IntPtr reserved, out IntPtr list);
    [DllImport("wlanapi.dll")]
    private static extern uint WlanCloseHandle(IntPtr client, IntPtr reserved);
    [DllImport("wlanapi.dll")]
    private static extern void WlanFreeMemory(IntPtr memory);
}
