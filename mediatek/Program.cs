// SPDX-License-Identifier: MIT
using System.Text.Json;

namespace Winawdl.MediaTek;

public static class Program
{
    private static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true, WriteIndented = true };
    private sealed record SavedSettings(string Sha256, string DriverVersion, Guid InterfaceId, bool AutoConfig, PhyRadio[] Radios);

    public static int Main(string[] args)
    {
        try
        {
            if (args is ["--self-test"]) return ProfileChecks.Run();
            if (args is not (["--probe", _] or ["--restore-settings", _] or ["--register-probe", _]))
            {
                Console.Error.WriteLine("Use --probe report.json, --restore-settings report.json, --register-probe report.json, or --self-test.");
                return 2;
            }
            var device = DeviceSnapshot.Read();
            var profile = DriverProfile.Find(Path.Combine(AppContext.BaseDirectory, "profiles"), device.DriverPath, device.DriverVersion)
                ?? throw new InvalidOperationException("MediaTek driver is unsupported. Install a reviewed program update.");
            if (!profile.AcceptsHardware(device.HardwareId))
                throw new InvalidOperationException("This MT7925 subsystem has not been validated.");
            using var wlan = new WlanSettings(device.InterfaceId);

            if (args[0] == "--restore-settings")
            {
                var saved = JsonSerializer.Deserialize<SavedSettings>(File.ReadAllText(args[1]), Json)
                    ?? throw new InvalidDataException("Saved WLAN settings are missing.");
                if (saved.InterfaceId != device.InterfaceId || saved.Sha256 != profile.Sha256 || saved.DriverVersion != profile.DriverVersion)
                    throw new InvalidOperationException("The saved settings belong to another adapter or driver.");
                wlan.Restore(saved.AutoConfig, saved.Radios);
                return 0;
            }

            using var control = new NativeControl(profile, device.DriverPath);
            var firmware = control.FirmwareVersion();
            if (args[0] == "--register-probe")
            {
                var rings = new List<object>();
                foreach (var queue in (uint[])[0, 1, 2, 3, 4, 5, 6, 15, 16, 17])
                {
                    var offset = 0x2300 + queue * 16;
                    rings.Add(new
                    {
                        queue,
                        @base = control.ReadDmaRegister(offset),
                        count = control.ReadDmaRegister(offset + 4),
                        cpu = control.ReadDmaRegister(offset + 8),
                        dma = control.ReadDmaRegister(offset + 12)
                    });
                }
                Write(args[1], new { firmware, profile.Id, global = control.ReadDmaRegister(0x2208), rings });
                return 0;
            }
            profile.CheckFirmware(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "System32", "drivers", "mtkwlan.dat"), firmware);
            Write(args[1], new
            {
                profile.Id,
                device.HardwareId,
                device.InterfaceId,
                device.DriverVersion,
                profile.Sha256,
                firmware,
                autoConfig = wlan.AutoConfig,
                radios = wlan.Radios
            });
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error.Message);
            return 3;
        }
    }

    private static void Write(string path, object value)
    {
        path = Path.GetFullPath(path);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, JsonSerializer.Serialize(value, Json));
    }
}
