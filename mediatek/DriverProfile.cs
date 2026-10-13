// SPDX-License-Identifier: MIT
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;

namespace Winawdl.MediaTek;

public sealed record DriverProfile(
    int Schema, string Id, string DriverVersion, string Sha256, string HardwarePrefix,
    string ControlDevice, uint FirmwareQueryOid, int FirmwareBufferBytes,
    int FirmwareVersionOffset, int FirmwareVersionBytes,
    int RadioLinkVersion,
    string FirmwareBundleSha256, string FirmwareVersion)
{
    private static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true };
    public bool AcceptsHardware(string hardwareId) =>
        hardwareId.Equals(HardwarePrefix, StringComparison.OrdinalIgnoreCase) ||
        hardwareId.StartsWith(HardwarePrefix + "&REV_", StringComparison.OrdinalIgnoreCase);

    public static DriverProfile? Find(string folder, string driverPath, string driverVersion)
    {
        if (!Directory.Exists(folder)) return null;
        var hash = Fingerprint(driverPath);
        foreach (var path in Directory.EnumerateFiles(folder, "*.json").Order())
        {
            var profile = JsonSerializer.Deserialize<DriverProfile>(File.ReadAllText(path), Json);
            if (profile is null || !profile.ValidLayout()) continue;
            if (profile.DriverVersion == driverVersion &&
                profile.Sha256.Equals(hash, StringComparison.OrdinalIgnoreCase)) return profile;
        }
        return null;
    }

    public static string Fingerprint(string path)
    {
        using var file = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(file)).ToLowerInvariant();
    }

    public bool ValidLayout() => Schema == 3 &&
        HardwarePrefix == @"PCI\VEN_14C3&DEV_7925&SUBSYS_E112105B" &&
        ControlDevice == @"\\.\MTKWLAN" &&
        FirmwareQueryOid == 0xfffcf007 && FirmwareBufferBytes == 100 &&
        FirmwareVersionOffset == 17 && FirmwareVersionBytes == 15 &&
        RadioLinkVersion == 3 &&
        Sha256.Length == 64 && Sha256.All(Uri.IsHexDigit) &&
        FirmwareBundleSha256 is { Length: 64 } && FirmwareBundleSha256.All(Uri.IsHexDigit) &&
        FirmwareVersion is { Length: 14 } && FirmwareVersion.All(char.IsAsciiDigit);

    public void CheckCurrentFile(string path)
    {
        if (!ValidLayout() || !Sha256.Equals(Fingerprint(path), StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("MediaTek driver changed; use a reviewed profile for the new version.");
    }

    public void CheckFirmware(string bundlePath, string loadedVersion)
    {
        if (!ValidLayout() || !FirmwareBundleSha256.Equals(Fingerprint(bundlePath), StringComparison.OrdinalIgnoreCase) ||
            loadedVersion != FirmwareVersion)
            throw new InvalidOperationException("MediaTek firmware changed; use a reviewed program update before starting the radio.");
    }
}
