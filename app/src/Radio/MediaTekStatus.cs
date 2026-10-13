// SPDX-License-Identifier: MIT
using System.IO;
using System.Text.Json;
using Winawdl.MediaTek;

namespace Winawdl.Radio;

public static class MediaTekStatus
{
    public const string Flag = "--mediatek-probe";

    public static bool Present()
    {
        try { DeviceSnapshot.Read(); return true; }
        catch (InvalidOperationException) { return false; }
    }

    public static int Write(string[] args)
    {
        if (args.Length != 1) return 2;
        object result;
        var status = 0;
        try
        {
            var device = DeviceSnapshot.Read();
            var profile = DriverProfile.Find(Path.Combine(AppContext.BaseDirectory, "profiles"), device.DriverPath, device.DriverVersion);
            if (profile is null || !profile.AcceptsHardware(device.HardwareId))
            {
                result = new
                {
                    device.DriverVersion,
                    supportedProfile = false,
                    readyForAirDrop = false,
                    reason = "Update the program for this MediaTek driver version."
                };
                status = 3;
            }
            else
            {
                using var control = new NativeControl(profile, device.DriverPath);
                result = new
                {
                    profile.Id,
                    device.DriverVersion,
                    profile.Sha256,
                    firmware = control.FirmwareVersion(),
                    controlProfileVerified = true
                };
            }
        }
        catch (Exception error)
        {
            result = new { readyForAirDrop = false, error = error.Message };
            status = 3;
        }
        var path = Path.GetFullPath(args[0]);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, JsonSerializer.Serialize(result, new JsonSerializerOptions { WriteIndented = true }));
        return status;
    }
}
