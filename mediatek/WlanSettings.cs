// SPDX-License-Identifier: MIT
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace Winawdl.MediaTek;

public sealed record PhyRadio(int Index, int Software, int Hardware);

/// <summary>An OEM WLAN snapshot. Restore software radio and
/// autoconfiguration through public WLAN APIs, including from a watchdog.</summary>
public sealed class WlanSettings : IDisposable
{
    private readonly IntPtr _client;
    private Guid _interface;
    public bool AutoConfig { get; }
    public PhyRadio[] Radios { get; }

    public WlanSettings(Guid id)
    {
        _interface = id;
        Check(WlanOpenHandle(2, IntPtr.Zero, out _, out _client));
        try
        {
            AutoConfig = BitConverter.ToInt32(Query(1), 0) != 0;
            Radios = ReadRadios();
            if (Radios.Any(radio => radio.Software is not (1 or 2) || radio.Hardware is not (1 or 2)))
                throw new InvalidDataException("WLAN radio state is unknown.");
        }
        catch { WlanCloseHandle(_client, IntPtr.Zero); throw; }
    }

    public void Restore()
        => Restore(AutoConfig, Radios);

    public void Restore(bool autoConfig, PhyRadio[] radios)
    {
        if (radios.Length is < 1 or > 64 || radios.Any(radio => radio.Software is not (1 or 2)))
            throw new InvalidDataException("Invalid saved WLAN settings.");
        try
        {
            var current = ReadRadios();
            if (!radios.Select(radio => radio.Index).Order().SequenceEqual(current.Select(radio => radio.Index).Order()))
                throw new InvalidOperationException("The PHY set changed; saved settings cannot be applied.");
            foreach (var radio in radios)
                if (current.FirstOrDefault(value => value.Index == radio.Index)?.Software != radio.Software)
                    SetRadio(radio, radio.Software);
        }
        finally
        {
            if ((BitConverter.ToInt32(Query(1), 0) != 0) != autoConfig)
                Set(1, BitConverter.GetBytes(autoConfig ? 1 : 0));
        }
    }

    private PhyRadio[] ReadRadios()
    {
        var state = Query(4);
        var count = BitConverter.ToInt32(state, 0);
        if (count <= 0 || count > 64 || state.Length < 4 + count * 12)
            throw new InvalidDataException("Invalid WLAN radio state.");
        return Enumerable.Range(0, count).Select(index => new PhyRadio(
            BitConverter.ToInt32(state, 4 + index * 12),
            BitConverter.ToInt32(state, 8 + index * 12),
            BitConverter.ToInt32(state, 12 + index * 12))).ToArray();
    }

    private void SetRadio(PhyRadio radio, int software)
    {
        var bytes = new byte[12];
        BitConverter.GetBytes(radio.Index).CopyTo(bytes, 0);
        BitConverter.GetBytes(software).CopyTo(bytes, 4);
        // The documented API ignores the hardware radio member.
        Set(4, bytes);
    }

    private byte[] Query(uint opcode)
    {
        Check(WlanQueryInterface(_client, ref _interface, opcode, IntPtr.Zero, out var length, out var data, out _));
        try
        {
            if (length < 4 || length > 4096) throw new InvalidDataException("Invalid WLAN reply length.");
            var bytes = new byte[length];
            Marshal.Copy(data, bytes, 0, bytes.Length);
            return bytes;
        }
        finally { WlanFreeMemory(data); }
    }

    private void Set(uint opcode, byte[] bytes) => Check(
        WlanSetInterface(_client, ref _interface, opcode, (uint)bytes.Length, bytes, IntPtr.Zero), $"WlanSetInterface({opcode})");
    private static void Check(uint error, string operation = "WLAN query")
    {
        if (error != 0) throw new Win32Exception((int)error, $"{operation}: Windows error {error}: {new Win32Exception((int)error).Message}");
    }
    public void Dispose() => WlanCloseHandle(_client, IntPtr.Zero);

    [DllImport("wlanapi.dll")]
    private static extern uint WlanOpenHandle(uint version, IntPtr reserved, out uint negotiated, out IntPtr client);
    [DllImport("wlanapi.dll")]
    private static extern uint WlanCloseHandle(IntPtr client, IntPtr reserved);
    [DllImport("wlanapi.dll")]
    private static extern uint WlanQueryInterface(IntPtr client, ref Guid id, uint opcode, IntPtr reserved,
        out uint length, out IntPtr data, out uint valueType);
    [DllImport("wlanapi.dll")]
    private static extern uint WlanSetInterface(IntPtr client, ref Guid id, uint opcode, uint length,
        [In] byte[] data, IntPtr reserved);
    [DllImport("wlanapi.dll")]
    private static extern void WlanFreeMemory(IntPtr memory);
}
