using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Windows.Threading;
using Winawdl.Model;

namespace Winawdl.Radio;

/// <summary>Starts and stops the radio from the app, which itself has no
/// administrator rights: it starts a second copy of itself that asks Windows
/// for them, and signals that copy through a named event.</summary>
public sealed class RadioControl : IRadio
{
    private const int CancelledByUser = 1223;
    private const int TailBytes = 4096;
    private const int MostShownChars = 280;

    private readonly string? _repo;
    private readonly DispatcherTimer _watch;
    private Process? _supervisor;
    private EventWaitHandle? _stop;
    private int _starts;
    private bool _installed;
    private DateTime _installedStartAt;
    private const string InstalledTask = "winawdl-MediaTek-Radio";
    private static readonly string InstalledState = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "winawdl", "radio");

    [System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    private static extern IntPtr OpenEventW(uint access, bool inherit, string name);
    [System.Runtime.InteropServices.DllImport("kernel32.dll")]
    private static extern bool SetEvent(IntPtr handle);
    [System.Runtime.InteropServices.DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);

    public RadioControl(Dispatcher dispatcher, string? repo)
    {
        _repo = repo;
        _installed = repo is not null && File.Exists(Path.Combine(repo, "installation.json"));
        _watch = new DispatcherTimer(TimeSpan.FromMilliseconds(400), DispatcherPriority.Background,
                                     (_, _) => Look(), dispatcher)
        { IsEnabled = false };
    }

    public RadioPhase Phase { get; private set; }
    public string LastLine { get; private set; } = "";
    public string LogFolder => _installed ? InstalledState : _repo is null ? "" : RadioSupervisor.LogFolder(_repo);
    public event Action? Changed;

    private void Become(RadioPhase phase, string lastLine = "")
    {
        if (Phase == phase && LastLine == lastLine) return;
        Phase = phase;
        LastLine = lastLine;
        Changed?.Invoke();
    }

    public void Start()
    {
        if (_installed)
        {
            _installedStartAt = DateTime.UtcNow;
            using var start = Process.Start(new ProcessStartInfo("schtasks.exe")
            {
                Arguments = $"/Run /TN {InstalledTask}",
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            });
            Become(RadioPhase.Starting);
            _watch.Start();
            return;
        }
        if (_supervisor is not null) return;
        try
        {
            if (MediaTekStatus.Present())
            {
                Become(RadioPhase.Failed, "AirDrop is not ready for this MediaTek Wi-Fi adapter.");
                return;
            }
        }
        catch (Exception error) when (error is Win32Exception or UnauthorizedAccessException or IOException)
        {
            Become(RadioPhase.Failed, error.Message);
            return;
        }
        if (_repo is null || !File.Exists(RadioSupervisor.RadioProgram(_repo))
                          || !File.Exists(RadioSupervisor.AwdlProgram(_repo)))
        {
            Become(RadioPhase.NotBuilt);
            return;
        }
        var stopName = $"winawdl-radio-stop-{Environment.ProcessId}-{++_starts}";
        _stop = new EventWaitHandle(false, EventResetMode.ManualReset, @"Local\" + stopName);
        try
        {
            _supervisor = Process.Start(new ProcessStartInfo(Environment.ProcessPath!)
            {
                UseShellExecute = true,
                Verb = "runas",
                Arguments = $"{RadioSupervisor.Flag} {Environment.ProcessId} {stopName}",
            });
        }
        catch (Win32Exception error) when (error.NativeErrorCode == CancelledByUser)
        {
            Become(RadioPhase.Refused);
            return;
        }
        catch (Win32Exception error)
        {
            Become(RadioPhase.Failed, error.Message);
            return;
        }
        Become(RadioPhase.Starting);
        _watch.Start();
    }

    public void Stop()
    {
        if (_installed)
        {
            var handle = OpenEventW(2, false, @"Global\winawdl-mt7925-stop");
            if (handle != IntPtr.Zero) { SetEvent(handle); CloseHandle(handle); }
            Become(RadioPhase.Stopping);
            return;
        }
        if (_supervisor is null) return;
        _stop?.Set();
        Become(RadioPhase.Stopping);
    }

    /// <summary>The engine saw the AWDL adapter come up or go away.</summary>
    public void Linked(bool up)
    {
        if (up && Phase == RadioPhase.Starting) Become(RadioPhase.Running);
    }

    private void Look()
    {
        if (_installed)
        {
            try
            {
                var active = OpenEventW(2, false, @"Global\winawdl-mt7925-stop");
                if (active != IntPtr.Zero) CloseHandle(active);
                using var doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(InstalledState, "radio-status.json")));
                var value = doc.RootElement;
                var state = value.GetProperty("state").GetString();
                if (state is "failed" or "recovery-failed")
                {
                    Become(RadioPhase.Failed, value.TryGetProperty("error", out var error) ? error.GetString() ?? "" : "Radio recovery needs attention.");
                    if (active == IntPtr.Zero) _watch.Stop();
                }
                else if (active == IntPtr.Zero && DateTime.UtcNow - _installedStartAt > TimeSpan.FromSeconds(15))
                {
                    Become(RadioPhase.Off); _watch.Stop();
                }
                else if (state is "preflight" or "switching") Become(RadioPhase.Starting);
                else if (state == "completed")
                {
                    if (Phase == RadioPhase.Stopping) { Become(RadioPhase.Off); _watch.Stop(); }
                }
                else if (state == "radio-session-running") Become(RadioPhase.Running);
            }
            catch (Exception error) when (error is IOException or JsonException) { }
            return;
        }
        if (_supervisor is null) return;
        bool ended;
        try
        {
            ended = _supervisor.WaitForExit(0);
        }
        catch (Exception error) when (error is Win32Exception or InvalidOperationException)
        {
            ended = true;
        }
        if (!ended)
        {
            if (Phase == RadioPhase.Starting) Become(RadioPhase.Starting, Tail("radio.log"));
            return;
        }
        _watch.Stop();
        _supervisor.Dispose();
        _supervisor = null;
        _stop?.Dispose();
        _stop = null;
        var by = EndedBy();
        if (Phase == RadioPhase.Stopping || by is "stopped" or "owner")
            Become(RadioPhase.Off);
        else
            Become(RadioPhase.Failed, Tail(by == "awdld" ? "awdld.log" : "radio.log"));
    }

    private string EndedBy()
    {
        try
        {
            using var status = JsonDocument.Parse(
                File.ReadAllText(Path.Combine(LogFolder, RadioSupervisor.StatusFile)));
            return status.RootElement.GetProperty("ended").GetString() ?? "";
        }
        catch (Exception error) when (error is IOException or JsonException or KeyNotFoundException)
        {
            return "";
        }
    }

    /// <summary>The last line a daemon wrote, which while it starts says what it
    /// is doing and after a failure says why.</summary>
    private string Tail(string logName)
    {
        try
        {
            using var log = new FileStream(Path.Combine(LogFolder, logName), FileMode.Open, FileAccess.Read,
                                           FileShare.ReadWrite | FileShare.Delete);
            log.Seek(-Math.Min(TailBytes, log.Length), SeekOrigin.End);
            var text = new StreamReader(log, Encoding.UTF8).ReadToEnd();
            var line = text.Split('\n').Select(part => part.Trim()).LastOrDefault(part => part.Length > 0) ?? "";
            return line.Length > MostShownChars ? line[..MostShownChars] + "…" : line;
        }
        catch (IOException)
        {
            return "";
        }
    }
}
