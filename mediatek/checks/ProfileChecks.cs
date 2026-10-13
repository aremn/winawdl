// SPDX-License-Identifier: MIT
using System.Text.Json;

namespace Winawdl.MediaTek;

internal static class ProfileChecks
{
    public static int Run()
    {
        var profile = DriverProfile.Find(Path.Combine(AppContext.BaseDirectory, "profiles"),
            Path.Combine(AppContext.BaseDirectory, "profiles", "mt7925-5.4.0.2974.json"), "5.4.0.2974");
        if (profile is not null) throw new Exception("A mismatched driver fingerprint was accepted.");
        var fixture = Path.Combine(AppContext.BaseDirectory, "profile-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(fixture);
        var driver = Path.Combine(fixture, "driver.bin");
        var firmware = Path.Combine(fixture, "mtkwlan.dat");
        var manifest = Path.Combine(fixture, "reviewed.json");
        try
        {
            File.WriteAllBytes(driver, [1, 2, 3, 4]);
            File.WriteAllBytes(firmware, [5, 6, 7, 8]);
            var original = JsonSerializer.Deserialize<DriverProfile>(File.ReadAllText(
                Path.Combine(AppContext.BaseDirectory, "profiles", "mt7925-5.4.0.2974.json")),
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true })!;
            var approved = original with
            {
                Sha256 = DriverProfile.Fingerprint(driver),
                FirmwareBundleSha256 = DriverProfile.Fingerprint(firmware)
            };
            File.WriteAllText(manifest, JsonSerializer.Serialize(approved));
            var found = DriverProfile.Find(fixture, driver, approved.DriverVersion)
                ?? throw new Exception("An exact reviewed fingerprint was not recognized.");
            if (!found.AcceptsHardware("PCI\\VEN_14C3&DEV_7925&SUBSYS_E112105B&REV_00"))
                throw new Exception("The supported subsystem was rejected.");
            if (found.AcceptsHardware("PCI\\VEN_14C3&DEV_7925&SUBSYS_00000000&REV_00"))
                throw new Exception("An unknown subsystem was accepted.");
            var device = new DeviceSnapshot("PCI\\synthetic\\instance", "PCI\\synthetic", approved.DriverVersion,
                driver, Guid.NewGuid(), 4);
            var saved = new SavedWlanSettings(approved.Sha256.ToUpperInvariant(), approved.DriverVersion,
                device.InstanceId, device.HardwareId, true, [new PhyRadio(0, 2, 1)]);
            saved.CheckAdapter(device with { InterfaceId = Guid.NewGuid() }, approved);
            ExpectWrongAdapter(saved with { InstanceId = "PCI\\synthetic\\other" }, device, approved);
            ExpectWrongAdapter(saved with { InstanceId = null! }, device, approved);
            ExpectWrongAdapter(saved with { HardwareId = "PCI\\other" }, device, approved);
            ExpectWrongAdapter(saved with { Sha256 = new string('0', 64) }, device, approved);
            ExpectWrongAdapter(saved with { DriverVersion = "5.4.0.2975" }, device, approved);
            approved.CheckFirmware(firmware, approved.FirmwareVersion);
            try { approved.CheckFirmware(firmware, "20241206164308"); throw new Exception("Changed loaded firmware was accepted."); }
            catch (InvalidOperationException) { }
            File.WriteAllBytes(firmware, [5, 6, 7, 9]);
            try { approved.CheckFirmware(firmware, approved.FirmwareVersion); throw new Exception("Changed firmware bundle was accepted."); }
            catch (InvalidOperationException) { }
            if ((approved with { Schema = 1 }).ValidLayout()) throw new Exception("Legacy profile without firmware binding was accepted.");
            if (DriverProfile.Find(fixture, driver, "5.4.0.2975") is not null)
                throw new Exception("A changed driver version was accepted.");
            File.WriteAllBytes(driver, [1, 2, 3, 5]);
            if (DriverProfile.Find(fixture, driver, approved.DriverVersion) is not null)
                throw new Exception("A changed driver binary was accepted.");
            try { approved.CheckCurrentFile(driver); throw new Exception("An update during a session was accepted."); }
            catch (InvalidOperationException) { }
        }
        finally
        {
            File.Delete(driver);
            File.Delete(firmware);
            File.Delete(manifest);
            Directory.Delete(fixture);
        }
        Console.WriteLine("PASS: exact fingerprint, unknown version, changed binary, update during session, changed firmware bundle/build, exact hardware match, WLAN restore across interface recreation with PCI identity checks.");
        return 0;
    }

    private static void ExpectWrongAdapter(SavedWlanSettings saved, DeviceSnapshot device, DriverProfile profile)
    {
        try { saved.CheckAdapter(device, profile); }
        catch (InvalidOperationException) { return; }
        throw new Exception("A WLAN snapshot from another adapter or driver was accepted.");
    }

}
