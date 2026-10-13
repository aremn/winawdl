using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Windows.Threading;
using Winawdl.Model;

namespace Winawdl.Engine;

/// <summary>Where the project's files are, seen from the running app.</summary>
public static class Repo
{
    /// <summary>The folder above the app that holds airdrop\engine.py, or null.</summary>
    public static string? Find()
    {
        for (var folder = new DirectoryInfo(AppContext.BaseDirectory); folder is not null; folder = folder.Parent)
            if (File.Exists(Path.Combine(folder.FullName, "airdrop", "engine.py")))
                return folder.FullName;
        return null;
    }

    public static string DataFolder
    {
        get
        {
            var folder = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "winawdl");
            Directory.CreateDirectory(folder);
            return folder;
        }
    }
}

/// <summary>Runs airdrop\engine.py and passes JSON lines both ways.</summary>
public sealed partial class EngineProcess(Dispatcher dispatcher, string? repo, IReadOnlyList<string> extraArguments,
                                          string? logDirectory = null) : IEngine
{
    [System.Text.RegularExpressions.GeneratedRegex(@"^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d,\d+ (?<level>[A-Z]+) (?<said>.*)$")]
    private static partial System.Text.RegularExpressions.Regex LogLine();

    private const long LogLimitBytes = 1024 * 1024;
    private static readonly UTF8Encoding Utf8 = new(encoderShouldEmitUTF8Identifier: false);

    private readonly object _gate = new();
    private Process? _process;
    private string _lastError = "";
    private long _lastErrorAt;

    public event Action<JsonElement>? Said;
    public event Action<string, string>? Stopped;

    public static string LogPath => Path.Combine(Repo.DataFolder, "engine.log");

    public int? ProcessId => _process?.Id;

    private static string? OnPath(string program) =>
        (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator)
            .Where(folder => folder.Length > 0)
            .Select(folder => Path.Combine(folder.Trim('"'), program))
            .FirstOrDefault(File.Exists);

    internal static string? FindPython(string? root)
    {
        var bundled = root is null ? null : Path.Combine(root, "runtime", "python", "python.exe");
        return bundled is not null && File.Exists(bundled) ? bundled
            : OnPath("python.exe") ?? OnPath("py.exe");
    }

    public void Start(string name, string folder, bool ask)
    {
        Stop();
        var script = repo is null ? null : Path.Combine(repo, "airdrop", "engine.py");
        if (script is null || !File.Exists(script))
        {
            Stopped?.Invoke("no_engine", "");
            return;
        }
        // The same choice the project's scripts make: the launcher first.
        var python = FindPython(repo);
        if (python is null)
        {
            Stopped?.Invoke("no_python", "");
            return;
        }
        var start = new ProcessStartInfo(python)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            WorkingDirectory = repo,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardInputEncoding = Utf8,
            StandardOutputEncoding = Utf8,
            StandardErrorEncoding = Utf8,
        };
        foreach (var argument in (string[])["-u", script, "--name", name, "--outdir", folder])
            start.ArgumentList.Add(argument);
        if (!ask) start.ArgumentList.Add("--take-all");
        foreach (var argument in extraArguments) start.ArgumentList.Add(argument);

        var log = OpenLog();
        var process = new Process { StartInfo = start, EnableRaisingEvents = true };
        process.ErrorDataReceived += (_, line) =>
        {
            if (string.IsNullOrWhiteSpace(line.Data)) return;
            // Its log is mostly routine. What explains an exit is an error, or
            // the unlogged lines Python prints as it gives up.
            var logged = LogLine().Match(line.Data);
            if (!logged.Success || logged.Groups["level"].Value is "ERROR" or "CRITICAL")
            {
                _lastError = logged.Success ? logged.Groups["said"].Value : line.Data.Trim();
                _lastErrorAt = Stopwatch.GetTimestamp();
            }
            lock (log) log.WriteLine(line.Data);
        };
        process.Exited += (_, _) => dispatcher.BeginInvoke(() =>
        {
            lock (log) log.Dispose();
            if (!ReferenceEquals(_process, process)) return;
            _process = null;
            // An error from long before the exit is not its cause.
            var recent = Stopwatch.GetElapsedTime(_lastErrorAt) < TimeSpan.FromSeconds(3);
            Stopped?.Invoke("ended", recent ? _lastError : "");
        });
        _lastError = "";
        try
        {
            process.Start();
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or IOException)
        {
            log.Dispose();
            Stopped?.Invoke("ended", error.Message);
            return;
        }
        _process = process;
        process.BeginErrorReadLine();
        _ = Task.Run(() => Read(process));
    }

    private StreamWriter OpenLog()
    {
        var path = logDirectory is null ? LogPath
            : Path.Combine(Directory.CreateDirectory(logDirectory).FullName, "engine.log");
        var old = new FileInfo(path);
        var keep = old.Exists && old.Length < LogLimitBytes;
        return new StreamWriter(new FileStream(path, keep ? FileMode.Append : FileMode.Create,
                                               FileAccess.Write, FileShare.ReadWrite), Utf8)
        {
            AutoFlush = true,
        };
    }

    private async Task Read(Process process)
    {
        while (await process.StandardOutput.ReadLineAsync() is { } line)
        {
            JsonElement said;
            try
            {
                using var parsed = JsonDocument.Parse(line);
                said = parsed.RootElement.Clone();
            }
            catch (JsonException)
            {
                continue;
            }
            _ = dispatcher.BeginInvoke(() =>
            {
                if (ReferenceEquals(_process, process)) Said?.Invoke(said);
            });
        }
    }

    public void Tell(Dictionary<string, object?> command)
    {
        var line = JsonSerializer.Serialize(command);
        lock (_gate)
        {
            try
            {
                _process?.StandardInput.WriteLine(line);
                _process?.StandardInput.Flush();
            }
            catch (IOException)
            {
                // The engine is ending; its exit is reported on its own.
            }
        }
    }

    public void Stop()
    {
        var process = _process;
        if (process is null) return;
        _process = null;
        try
        {
            // The end of its input is the engine's cue to leave cleanly.
            lock (_gate) process.StandardInput.Close();
            if (!process.WaitForExit(3000)) process.Kill(entireProcessTree: true);
        }
        catch (Exception error) when (error is IOException or InvalidOperationException)
        {
        }
    }
}
