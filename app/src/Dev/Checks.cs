using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Automation.Peers;
using System.Windows.Automation.Provider;
using System.Windows.Documents;
using Winawdl.Engine;
using Winawdl.Model;
using Winawdl.Radio;
using Winawdl.Theme;
using Winawdl.Views;

namespace Winawdl.Dev;

/// <summary>The checks that need neither a radio nor the engine: the colors'
/// contrast for any accent, the wording, what each state of the radio and the
/// engine puts on screen, and the supervisor that runs the two daemons.</summary>
public static class Checks
{
    public const string Flag = "--check";

    private static StreamWriter _log = null!;
    private static int _passed, _failed;

    public static int Run(string[] args)
    {
        if (args.Length < 1) return 2;
        var folder = Directory.CreateDirectory(args[0]).FullName;
        using var log = new StreamWriter(Path.Combine(folder, "check.log")) { AutoFlush = true };
        _log = log;
        try
        {
            Colors();
            Wording();
            var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
            Program.Dress(app).Apply(Palette.Light(Rgb.Hex("#0078D4")));
            Motion.Reduced = true;
            States(app);
            Supervisor(Path.Combine(folder, "supervised"));
        }
        catch (Exception error)
        {
            log.WriteLine($"STOPPED {error}");
            return 1;
        }
        log.WriteLine(_failed == 0 ? $"ALL {_passed} PASSED" : $"{_failed} FAILED, {_passed} passed");
        return _failed == 0 ? 0 : 1;
    }

    private static void Expect(bool ok, string what)
    {
        if (ok) _passed++;
        else _failed++;
        _log.WriteLine($"{(ok ? "PASS" : "FAIL")}  {what}");
    }

    /// <summary>The 48 accents Windows offers, and the extremes nobody offers.</summary>
    private static readonly string[] Accents =
    [
        "FFB900", "FF8C00", "F7630C", "CA5010", "DA3B01", "EF6950", "D13438", "FF4343", "E74856", "E81123",
        "EA005E", "C30052", "E3008C", "BF0077", "C239B3", "9A0089", "0078D4", "0063B1", "8E8CD8", "6B69D6",
        "8764B8", "744DA9", "B146C2", "881798", "0099BC", "2D7D9A", "00B7C3", "038387", "00B294", "018574",
        "00CC6A", "10893E", "7A7574", "5D5A58", "68768A", "515C6B", "567C73", "486860", "498205", "107C10",
        "767676", "4C4A48", "69797E", "4A5459", "647C64", "525E54", "847545", "7E735F",
        "FFFFFF", "000000", "FFFF00", "0000FF", "808080",
    ];

    private static void Colors()
    {
        double text = double.MaxValue, onFill = double.MaxValue, accentText = double.MaxValue,
               fill = double.MaxValue, tinted = double.MaxValue;
        foreach (var accent in Accents.Select(Rgb.Hex))
            foreach (var palette in (Palette[])[Palette.Light(accent), Palette.Night(accent)])
            {
                foreach (var surface in palette.Surfaces)
                {
                    foreach (var ink in (Rgb[])[palette.Ink, palette.InkSoft, palette.Ember])
                        text = Math.Min(text, Rgb.Contrast(ink, surface));
                    accentText = Math.Min(accentText, Rgb.Contrast(palette.AccentText, surface));
                    fill = Math.Min(fill, Rgb.Contrast(palette.Accent, surface));
                }
                onFill = Math.Min(onFill, Rgb.Contrast(palette.OnAccent, palette.Accent));
                tinted = Math.Min(tinted, Math.Min(Rgb.Contrast(palette.Ink, palette.AccentSoft),
                                                   Rgb.Contrast(palette.InkSoft, palette.AccentSoft)));
            }
        var cases = $"{Accents.Length} accents, light and dark";
        Expect(text >= Palette.TextContrast, $"ink, soft ink and ember on every surface: lowest {text:0.00} to 1");
        Expect(onFill >= Palette.TextContrast, $"text on an accent fill, {cases}: lowest {onFill:0.00} to 1");
        Expect(accentText >= Palette.TextContrast, $"accent as text or focus ring on every surface, {cases}: lowest {accentText:0.00} to 1");
        Expect(fill >= Palette.ShapeContrast, $"accent as a fill against every surface, {cases}: lowest {fill:0.00} to 1");
        Expect(tinted >= Palette.TextContrast, $"ink on the tint of a transfer in progress, {cases}: lowest {tinted:0.00} to 1");
    }

    private static void Wording()
    {
        Expect(Words.Size(512) == "512 bytes" && Words.Size(1) == "1 byte" && Words.Size(26_100_000) == "24.9 MB"
               && Words.Size(3 * 1024L * 1024 * 1024) == "3.00 GB" && Words.Size(20_000) == "20 KB",
               "sizes read as Windows writes them");
        Expect(Words.Things(["a.jpg"]) == "a photo" && Words.Things(["a.jpg", "b.HEIC"]) == "2 photos"
               && Words.Things(["a.mov"]) == "a video" && Words.Things(["a.jpg", "b.mov"]) == "2 photos and videos"
               && Words.Things(["a.pdf", "b.jpg"]) == "2 files" && Words.Things([], 1) == "a link"
               && Words.Things([], 3) == "3 links" && Words.Things([]) == "something",
               "what is being sent is named by its kind");
        Expect(Words.KindOfDevice("iPhone14,2") == DeviceKind.Phone && Words.KindOfDevice("iPad13,1") == DeviceKind.Tablet
               && Words.KindOfDevice("MacBookPro18,3") == DeviceKind.Computer && Words.KindOfDevice("Macmini9,1") == DeviceKind.Computer
               && Words.KindOfDevice(null) == DeviceKind.Other && Words.KindOfDevice("RealityDevice14,1") == DeviceKind.Other,
               "a device's kind comes from the model it reports");
        Expect(Words.Percent(0.999) == "99%" && Words.Percent(1) == "100%" && Words.Percent(-1) == "0%",
               "a percentage never shows 100 before the end");
    }

    private static MainView Show(Stage.World world)
    {
        var view = new MainView { DataContext = world.Model };
        var host = new AdornerDecorator { Child = view };
        host.Measure(new Size(460, 720));
        host.Arrange(new Rect(0, 0, 460, 720));
        host.UpdateLayout();
        return view;
    }

    private static bool Press(MainView view, string name)
    {
        view.UpdateLayout();
        var control = Stage.Controls(view).FirstOrDefault(one => one.Line.Contains($"\"{name}\"")).Control;
        if (control is null) return false;
        var peer = UIElementAutomationPeer.CreatePeerForElement(control);
        if (peer.GetPattern(PatternInterface.Invoke) is IInvokeProvider invoke) invoke.Invoke();
        else if (peer.GetPattern(PatternInterface.Toggle) is IToggleProvider toggle) toggle.Toggle();
        else return false;
        Stage.Pump(view.Dispatcher, 80);
        return true;
    }

    private static void States(Application app)
    {
        // The radio, from off to on and through each way it can fail.
        var world = Stage.NewWorld(app.Dispatcher);
        var view = Show(world);
        var model = world.Model;
        Expect(model.Across == Across.RadioOff && model.Presence == "Not visible" && !model.RadioOn,
               $"no link: \"{model.PanelTitle}\", header \"{model.Presence}\"");
        Expect(Press(view, "Turn on the radio") && world.Radio.Starts == 1 && model.Across == Across.RadioStarting && model.RadioOn,
               $"Turn on the radio -> the radio is started once: \"{model.PanelTitle}\"");
        world.Radio.Become(RadioPhase.Starting, "radio: card ready");
        Expect(model.PanelBody == "radio: card ready", "while it starts, the panel shows what the radio last wrote");
        world.Engine.Say(new { @event = "link", up = true });
        world.Engine.Say(new { @event = "receiving", on = true });
        Expect(model.Across == Across.Nearby && world.Radio.Phase == RadioPhase.Running && model.Presence == "Visible as STUDY-PC",
               $"link up -> the devices' side, header \"{model.Presence}\", \"{model.NobodyTitle}\"");
        world.Engine.Say(new { @event = "looking", on = true, beacon = false, why = "the radio is off" });
        world.Engine.Say(new { @event = "unwilling", count = 2 });
        world.Engine.Say(new { @event = "receiving", on = false, reason = "port_held" });
        Expect(model.Notices.Count == 3 && model.Presence == "Not visible yet",
               $"no Bluetooth, a held port and shy devices -> three notices: {string.Join(" | ", model.Notices)}");
        world.Engine.Say(new { @event = "unwilling", count = 1 });
        Expect(model.Notices.Contains("A device nearby only answers devices it knows. Set its AirDrop to Everyone for 10 Minutes."),
               "one device that refuses this PC or keeps its name -> the notice says what to change on it");
        world.Engine.Say(new { @event = "unwilling", count = 0 });
        Expect(model.Notices.Count == 2, "it stops refusing, or leaves -> the notice goes");
        world.Engine.Say(new { @event = "link", up = false });
        world.Radio.Become(RadioPhase.Failed, "radio: could not get 6 locked 2 MiB blocks below 4 GiB");
        Expect(model.Across == Across.RadioProblem && model.PanelTitle == "The radio stopped" && model.PanelHasLog,
               $"the radio ends by itself -> \"{model.PanelTitle}\": \"{model.PanelBody}\"");
        Expect(Press(view, "Open the logs") && world.Desk.Opened.Last().StartsWith("open "), "Open the logs -> the log folder is opened (recorded)");
        Expect(Press(view, "Try again") && world.Radio.Starts == 2, "Try again -> the radio is started again");
        world.Radio.Become(RadioPhase.Failed, "");
        Expect(model.PanelTitle == "The radio did not start" && model.PanelBody == "It left nothing in its log.",
               $"it fails before running, silent -> \"{model.PanelTitle}\": \"{model.PanelBody}\"");
        world.Radio.Become(RadioPhase.Refused);
        Expect(model.PanelTitle == "The radio stayed off" && !model.PanelHasLog && !model.RadioOn,
               $"Windows was told no -> \"{model.PanelTitle}\": \"{model.PanelBody}\"");
        world.Radio.Become(RadioPhase.NotBuilt);
        Expect(model.PanelBody.Contains("build-radio.ps1"), $"nothing built -> \"{model.PanelTitle}\": \"{model.PanelBody}\"");

        // The engine, each way it can be missing.
        foreach (var (why, detail, title) in new[]
                 {
                     ("no_python", "", "Python was not found"), ("no_engine", "", "winawdl's files are incomplete"),
                     ("no_certificate", "OpenSSL not found", "No certificate could be made"),
                     ("ended", "", "The AirDrop engine stopped"),
                 })
        {
            world.Engine.End(why, detail);
            Expect(model.Across == Across.EngineDown && model.PanelTitle == title && model.PanelBody.Length > 20 || detail.Length > 0 && model.PanelBody == detail,
                   $"engine {why} -> \"{model.PanelTitle}\": \"{model.PanelBody}\"");
        }
        var before = world.Engine.Told.Count;
        Expect(Press(view, "Start it again") && model.Across != Across.EngineDown && world.Engine.Told.Count > before,
               "Start it again -> the engine is started and told whether to look");

        // The switch does what the panel's button does, and takes it back.
        world = Stage.NewWorld(app.Dispatcher);
        view = Show(world);
        model = world.Model;
        Expect(Press(view, "Radio") && world.Radio.Starts == 1 && model.RadioOn, "the Radio switch, off -> the radio is started");
        Expect(Press(view, "Radio") && world.Radio.Phase == RadioPhase.Off && !model.RadioOn, "the Radio switch, on -> the radio the app started is stopped");

        // A transfer that the device walks away from, and one that is lost on the way in.
        world = Stage.NewWorld(app.Dispatcher);
        view = Show(world);
        model = world.Model;
        var copied = Stage.SampleFiles(Path.Combine(Path.GetTempPath(), "winawdl-check-files"));
        world.Desk.Picked = copied[..2];
        model.Paste.Execute(null);
        Expect(model.Held.Count == 2, "the command behind Ctrl+V takes the files on the clipboard in hand (the keys were not pressed)");
        model.ClearHeld.Execute(null);
        world.Engine.Say(new { @event = "link", up = true });
        world.Engine.Say(new { @event = "peer", id = "a", name = "Stand-in iPhone", model = "iPhone14,2" });
        var files = Stage.SampleFiles(Path.Combine(Path.GetTempPath(), "winawdl-check-files"));
        world.Desk.Picked = files[..2];
        Expect(Press(view, "Stand-in iPhone") && model.Held.Count == 2 && world.Engine.Told.Last().Contains("\"do\":\"send\""),
               "press a device with nothing in hand -> files are chosen and sent to it in one go");
        world.Engine.Say(new { @event = "peer_gone", id = "a" });
        Expect(model.Peers.Count == 1, "it stops announcing itself mid-transfer -> its plate stays until the transfer ends");
        world.Engine.Say(new { @event = "send", id = "s1", state = "failed", reason = "no_answer" });
        Expect(model.Peers.Count == 0, "the transfer fails -> the plate of the device that left goes too");
        world.Engine.Say(new { @event = "asked", id = "r1", sender = "", files = Array.Empty<object>(), links = 2, waits = true });
        Expect(model.Incoming[0].Headline == "A device nearby wants to send you 2 links", $"a nameless sender with links -> \"{model.Incoming[0].Headline}\"");
        world.Engine.Say(new { @event = "answered", id = "r1", how = "left" });
        Expect(model.Incoming.Count == 0, "the sender stops waiting -> its request goes by itself");
        world.Engine.Say(new { @event = "asked", id = "r2", sender = "Stand-in iPhone", files = new[] { new { name = "a.mov", size = 5, type = "" } }, links = 0, total = 5, waits = false });
        world.Engine.Say(new { @event = "lost", id = "r2", reason = "nothing arrived" });
        Expect(model.Incoming[0].IsLost && Press(view, "Dismiss") && model.Incoming.Count == 0,
               "a transfer lost on the way in says so, and Dismiss removes it");
    }

    private static void Supervisor(string logs)
    {
        // Ping stands in for both daemons: it runs until interrupted, and says so when it is.
        var ping = Path.Combine(Environment.SystemDirectory, "ping.exe");
        Daemon Forever(string name) => new(name, ping, ["-t", "127.0.0.1"]);

        (int Code, double Seconds, string Ended) Supervised(int owner, Daemon radio, Daemon awdl, Action<EventWaitHandle> during)
        {
            var name = $"winawdl-check-{Environment.ProcessId}-{Guid.NewGuid():N}";
            using var stop = new EventWaitHandle(false, EventResetMode.ManualReset, @"Local\" + name);
            var clock = Stopwatch.StartNew();
            var run = Task.Run(() => RadioSupervisor.Supervise(owner, name, radio, awdl, logs));
            during(stop);
            var code = run.Wait(TimeSpan.FromSeconds(30)) ? run.Result : -1;
            var status = Path.Combine(logs, RadioSupervisor.StatusFile);
            return (code, clock.Elapsed.TotalSeconds, File.Exists(status) ? File.ReadAllText(status) : "");
        }

        var asked = Supervised(Environment.ProcessId, Forever("radio"), Forever("awdld"), stop =>
        {
            Thread.Sleep(2500);
            stop.Set();
        });
        var said = File.ReadAllText(Path.Combine(logs, "awdld.log"));
        // Windows localizes "Reply from"; TTL is still present on a reply.
        Expect(asked.Code == 0 && asked.Ended.Contains("stopped") && said.Contains("TTL="),
               $"asked to stop -> both daemons end, exit {asked.Code}, their output is in the logs");
        Expect(said.Contains("Control-C") && asked.Seconds < 2.5 + 4,
               $"they are interrupted the way Ctrl+C does, not killed: stopped {asked.Seconds - 2.5:0.0} s after the request");
        Expect(Process.GetProcessesByName("PING").Length == 0, "no daemon is left running");

        // A daemon that says one line with a dash in it on its error stream, then stays.
        var dashed = $"up {(char)0x2014} ready";
        var speaker = new Daemon("radio", Path.Combine(Environment.SystemDirectory, "WindowsPowerShell", "v1.0", "powershell.exe"),
        [
            "-NoProfile", "-Command",
            "$e = [Console]::OpenStandardError(); $b = [Text.Encoding]::UTF8.GetBytes(\"" + dashed + "`n\"); "
            + "$e.Write($b, 0, $b.Length); $e.Flush(); Start-Sleep 30",
        ]);
        var spoken = Supervised(Environment.ProcessId, speaker, Forever("awdld"), stop =>
        {
            Thread.Sleep(4000);
            stop.Set();
        });
        Expect(spoken.Code == 0 && File.ReadAllText(Path.Combine(logs, "radio.log")).Contains(dashed),
               "what a daemon writes to its error stream in UTF-8 is in its log as written");

        var died = Supervised(Environment.ProcessId, new Daemon("radio", ping, ["-n", "1", "127.0.0.1"]), Forever("awdld"), _ => { });
        Expect(died.Code == 3 && died.Ended.Contains("radio"), $"a daemon ends by itself -> exit {died.Code}, the status names it, the other is stopped");

        using var owner = Process.Start(new ProcessStartInfo(ping, "-n 3 127.0.0.1") { CreateNoWindow = true, UseShellExecute = false })!;
        var orphaned = Supervised(owner.Id, Forever("radio"), Forever("awdld"), _ => { });
        Expect(orphaned.Code == 0 && orphaned.Ended.Contains("owner"), $"the app is gone -> the daemons are stopped, exit {orphaned.Code}");

        var missing = Supervised(Environment.ProcessId, new Daemon("radio", Path.Combine(logs, "no-such.exe"), []), Forever("awdld"), _ => { });
        Expect(missing.Code == 3 && missing.Ended.Contains("launch"), $"a daemon that cannot be started -> exit {missing.Code}, nothing left running");
        Expect(Process.GetProcessesByName("PING").Length == 0, "and again no daemon is left running");

        var noRepo = new EngineProcess(System.Windows.Threading.Dispatcher.CurrentDispatcher, null, []);
        string? why = null;
        noRepo.Stopped += (reason, _) => why = reason;
        noRepo.Start("x", Path.GetTempPath(), true);
        Expect(why == "no_engine", "an app that cannot find its engine says so and starts nothing");
    }
}
