// SPDX-License-Identifier: MIT
using System.IO;
using System.Buffers.Binary;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace Winawdl.MediaTek;

public sealed class NativeControl : IDisposable
{
    private const uint QueryGlobalStats = 0x170002;
    private readonly SafeFileHandle _handle;
    private readonly DriverProfile _profile;
    private readonly string _driverPath;

    public NativeControl(DriverProfile profile, string driverPath)
    {
        profile.CheckCurrentFile(driverPath);
        _profile = profile;
        _driverPath = driverPath;
        _handle = CreateFile(profile.ControlDevice, 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (_handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(), "Open MediaTek control device");
    }

    public string FirmwareVersion()
    {
        var bytes = Query(_profile.FirmwareQueryOid, new byte[_profile.FirmwareBufferBytes]);
        var version = bytes.AsSpan(_profile.FirmwareVersionOffset, _profile.FirmwareVersionBytes);
        var zero = version.IndexOf((byte)0);
        if (zero >= 0) version = version[..zero];
        if (version.IsEmpty || !version.ToArray().All(value => value is >= (byte)'0' and <= (byte)'9'))
            throw new InvalidDataException("Firmware reply does not match the reviewed ABI.");
        return System.Text.Encoding.ASCII.GetString(version);
    }

    public uint ReadDmaRegister(uint offset)
    {
        // FuncGetCr uses an eight-byte {offset,value} buffer. Only inspected
        // ring configuration and global configuration registers are exposed;
        // interrupt status and other registers with read side effects are not.
        if (offset != 0x2208 && !(offset >= 0x2300 && offset <= 0x241c && (offset & 3) == 0))
            throw new ArgumentOutOfRangeException(nameof(offset));
        var bytes = new byte[8];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, offset);
        var reply = Query(0xfffcf001, bytes);
        return BinaryPrimitives.ReadUInt32LittleEndian(reply.AsSpan(4));
    }

    private byte[] Query(uint oid, byte[] response)
    {
        _profile.CheckCurrentFile(_driverPath);
        // METHOD_OUT_DIRECT: the driver reads command parameters from the
        // initialized output buffer. Input contains only the four-byte OID.
        var input = BitConverter.GetBytes(oid);
        if (!DeviceIoControl(_handle, QueryGlobalStats, input, input.Length, response, response.Length,
                             out var returned, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error(), $"MediaTek query 0x{oid:x8}");
        if (returned != response.Length)
            throw new InvalidDataException($"Unexpected MediaTek reply length: {returned}.");
        return response;
    }

    public void Dispose() => _handle.Dispose();

    [DllImport("kernel32.dll", EntryPoint = "CreateFileW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security,
                                                   uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle device, uint ioctl,
        [In] byte[] input, int inputBytes, [In, Out] byte[] output, int outputBytes,
        out int returned, IntPtr overlapped);
}
