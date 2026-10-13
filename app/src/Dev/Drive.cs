using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Automation.Peers;
using System.Windows.Automation.Provider;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Winawdl.Engine;
using Winawdl.Model;
using Winawdl.Theme;
using Winawdl.Views;

namespace Winawdl.Dev;

/// <summary>A second engine, driven line by line, that plays the device at the
/// other end of a transfer.</summary>
internal sealed class OtherDevice : IDisposable
{
    private readonly Process _process;
    private readonly ConcurrentQueue<JsonElement> _said = new();
    private readonly ConcurrentQueue<string> _errors = new();
    private readonly List<JsonElement> _seen = [];

    public OtherDevice(string python, string repo, string name, int port, int peerPort, string folder)
    {
        var start = new ProcessStartInfo(python)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardInput = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardInputEncoding = new UTF8Encoding(false),
        };
        foreach (var argument in (string[])
                 [
                     "-u", Path.Combine(repo, "airdrop", "engine.py"), "--loopback", "--port", $"{port}",
                     "--peer", $"[::1]:{peerPort}", "--name", name, "--outdir", folder,
                     "--keys", Path.Combine(folder, "keys"),
                 ])
            start.ArgumentList.Add(argument);
        _process = Process.Start(start)!;
        _process.ErrorDataReceived += (_, line) =>
        {
            if (!string.IsNullOrWhiteSpace(line.Data)) _errors.Enqueue(line.Data);
        };
        _process.BeginErrorReadLine();
        _ = Task.Run(async () =>
        {
            while (await _process.StandardOutput.ReadLineAsync() is { } line)
            {
                using var parsed = JsonDocument.Parse(line);
                _said.Enqueue(parsed.RootElement.Clone());
            }
        });
    }

    public void Tell(object command)
    {
        _process.StandardInput.WriteLine(JsonSerializer.Serialize(command));
        _process.StandardInput.Flush();
    }

    public string FailureDetails => string.Join(Environment.NewLine, _errors);

    /// <summary>The first event not returned before that has these fields, or
    /// null if none comes in time.</summary>
    public JsonElement? Heard(Func<Action, double, bool> until, string @event, params (string Key, string Value)[] with)
    {
        JsonElement? found = null;
        until(() =>
        {
            while (_said.TryDequeue(out var said)) _seen.Add(said);
            for (var index = 0; index < _seen.Count && found is null; index++)
            {
                var said = _seen[index];
                if (Text(said, "event") != @event || with.Any(pair => Text(said, pair.Key) != pair.Value)) continue;
                found = said;
                _seen.RemoveAt(index);
            }
            if (found is null) throw new NotYet();
        }, 20);
        return found;
    }

    public static string Text(JsonElement said, string name) =>
        said.TryGetProperty(name, out var value)
            ? value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : value.ToString()
            : "";

    public void Dispose()
    {
        try
        {
            _process.StandardInput.Close();
            if (!_process.WaitForExit(3000)) _process.Kill(entireProcessTree: true);
        }
        catch (Exception error) when (error is IOException or InvalidOperationException)
        {
        }
    }
}

internal sealed class NotYet : Exception;

/// <summary>Asks Windows for a window's picture as the compositor has it, frame
/// and title bar included, wherever the window is.</summary>
internal static class Screen
{
    private const uint FullContent = 2;

    public static bool Print(Window window, string path)
    {
        var handle = new System.Windows.Interop.WindowInteropHelper(window).Handle;
        if (!GetWindowRect(handle, out var bounds)) return false;
        using var bitmap = new System.Drawing.Bitmap(bounds.Right - bounds.Left, bounds.Bottom - bounds.Top);
        using var graphics = System.Drawing.Graphics.FromImage(bitmap);
        var context = graphics.GetHdc();
        var printed = PrintWindow(handle, context, FullContent);
        graphics.ReleaseHdc(context);
        bitmap.Save(path, System.Drawing.Imaging.ImageFormat.Png);
        return printed;
    }

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct Edges
    {
        public int Left, Top, Right, Bottom;
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr window, out Edges bounds);

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool PrintWindow(IntPtr window, IntPtr context, uint flags);
}

/// <summary>Draws a control's own keyboard focus ring on it, the way WPF does
/// when Tab reaches it. Pressing Tab for real would take the keyboard.</summary>
internal sealed class RingAdorner : Adorner
{
    private readonly Control _ring;

    public RingAdorner(Control adorned) : base(adorned)
    {
        _ring = new Control { Style = adorned.FocusVisualStyle, IsHitTestVisible = false };
        AddVisualChild(_ring);
    }

    protected override int VisualChildrenCount => 1;
    protected override Visual GetVisualChild(int index) => _ring;

    protected override Size ArrangeOverride(Size size)
    {
        _ring.Arrange(new Rect(size));
        return size;
    }
}

/// <summary>Runs the app's real window, out of sight and never activated, against
/// the real engine over loopback, pressing each control through the same
/// interface a screen reader uses, and writes down what happened with a
/// picture of each step.</summary>
public static class Drive
{
    public const string Flag = "--drive";

    public static int Run(string[] args)
    {
        if (args.Length < 1) return 2;
        var folder = Directory.CreateDirectory(args[0]).FullName;
        using var log = new StreamWriter(Path.Combine(folder, "drive.log")) { AutoFlush = true };
        try
        {
            return new Session(folder, log).Go();
        }
        catch (Exception error)
        {
            log.WriteLine($"STOPPED {error}");
            return 1;
        }
    }

    private sealed class Session(string folder, StreamWriter log)
    {
        private readonly Application _app = new() { ShutdownMode = ShutdownMode.OnExplicitShutdown };
        private ThemeService _theme = null!;
        private TableModel _model = null!;
        private MainWindow _window = null!;
        private MainView _view = null!;
        private ScriptedDesk _desk = null!;
        private EngineProcess _engine = null!;
        private OtherDevice _other = null!;
        private AppSettings _settings = null!;
        private int _failed, _shots;

        private void Say(string line) => log.WriteLine(line);

        private void Check(bool ok, string what)
        {
            if (!ok) _failed++;
            Say($"{(ok ? "PASS" : "FAIL")}  {what}");
        }

        private void Pump(int milliseconds) => Stage.Pump(_app.Dispatcher, milliseconds);

        private bool Until(Func<bool> condition, double seconds = 15)
        {
            var clock = Stopwatch.StartNew();
            while (!condition())
            {
                if (clock.Elapsed.TotalSeconds > seconds) return false;
                Pump(30);
            }
            Pump(60);
            return true;
        }

        private bool Trying(Action attempt, double seconds) => Until(() =>
        {
            try
            {
                attempt();
                return true;
            }
            catch (NotYet)
            {
                return false;
            }
        }, seconds);

        private Control? Named(string name) => Stage.Controls(_view)
            .FirstOrDefault(one => one.Line.Contains($"\"{name}\"") && one.Control.IsEnabled).Control;

        /// <summary>Presses a control by its spoken name through its automation
        /// peer: what Narrator's "activate" does, with no pointer involved.</summary>
        private bool Press(string name)
        {
            if (Named(name) is not { } control)
            {
                Check(false, $"press \"{name}\": no such control in reach");
                return false;
            }
            var peer = UIElementAutomationPeer.CreatePeerForElement(control);
            if (peer.GetPattern(PatternInterface.Invoke) is IInvokeProvider invoke) invoke.Invoke();
            else if (peer.GetPattern(PatternInterface.SelectionItem) is ISelectionItemProvider select) select.Select();
            else if (peer.GetPattern(PatternInterface.Toggle) is IToggleProvider toggle) toggle.Toggle();
            else
            {
                Check(false, $"press \"{name}\": it offers nothing to press");
                return false;
            }
            Pump(120);
            return true;
        }

        private void Key(UIElement on, Key key)
        {
            on.RaiseEvent(new KeyEventArgs(Keyboard.PrimaryDevice, PresentationSource.FromVisual(on)!, 0, key)
            {
                RoutedEvent = Keyboard.KeyDownEvent,
            });
            Pump(120);
        }

        private string Shot(string name)
        {
            _view.UpdateLayout();
            var root = (FrameworkElement)VisualTreeHelper.GetChild(_window, 0);
            var bitmap = new RenderTargetBitmap((int)(root.ActualWidth * 2), (int)(root.ActualHeight * 2), 192, 192,
                                                PixelFormats.Pbgra32);
            bitmap.Render(root);
            var path = Path.Combine(folder, $"{++_shots:00}-{name}.png");
            Stage.WritePng(bitmap, path);
            Say($"      picture {Path.GetFileName(path)}");
            return path;
        }

        /// <summary>Whether anything on screen reaches past the window's sides,
        /// leaving out what sits in a strip made to scroll sideways.</summary>
        private List<string> Overflowing()
        {
            var wide = new List<string>();
            void Walk(DependencyObject parent, bool scrolls)
            {
                for (var index = 0; index < VisualTreeHelper.GetChildrenCount(parent); index++)
                {
                    if (VisualTreeHelper.GetChild(parent, index) is not FrameworkElement child) continue;
                    if (child.Visibility != Visibility.Visible) continue;
                    var inside = scrolls || child is ScrollViewer
                    {
                        HorizontalScrollBarVisibility: not ScrollBarVisibility.Disabled,
                    };
                    if (!inside && child.ActualWidth > 0)
                    {
                        var bounds = child.TransformToAncestor(_view).TransformBounds(new Rect(child.RenderSize));
                        if (bounds.Left < -2 || bounds.Right > _view.ActualWidth + 2)
                            wide.Add($"{child.GetType().Name} {bounds.Left:0}..{bounds.Right:0}");
                    }
                    Walk(child, inside);
                }
            }
            Walk(_view, false);
            return wide;
        }

        private static int FreePort()
        {
            using var listener = new TcpListener(IPAddress.IPv6Loopback, 0);
            listener.Start();
            return ((IPEndPoint)listener.LocalEndpoint).Port;
        }

        private static string Hash(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));

        private string Status(PeerModel peer) => $"{peer.State}: {peer.Status}";

        public int Go()
        {
            var repo = Repo.Find() ?? throw new InvalidOperationException("the repository was not found");
            var python = EngineProcess.FindPython(repo)
                ?? throw new InvalidOperationException("Python is required for the loopback scenario.");
            var mine = Directory.CreateDirectory(Path.Combine(folder, "mine")).FullName;
            var theirs = Directory.CreateDirectory(Path.Combine(folder, "theirs")).FullName;
            foreach (var old in Directory.GetFiles(mine).Concat(Directory.GetFiles(theirs))) File.Delete(old);
            var files = Stage.SampleFiles(Path.Combine(folder, "files"));
            var big = Path.Combine(folder, "files", "stand-in-film.mov");
            if (!File.Exists(big)) File.WriteAllBytes(big, RandomNumberGenerator.GetBytes(96 * 1024 * 1024));
            int myPort = FreePort(), theirPort = FreePort();

            _theme = Program.Dress(_app);
            _theme.Apply(Palette.Night(ThemeService.SystemAccent()));
            Motion.Reduced = true;
            _desk = new ScriptedDesk(_app.Dispatcher) { RunsLater = true };
            _settings = new AppSettings { Name = "Driven PC", Folder = mine };
            _engine = new EngineProcess(_app.Dispatcher, repo,
                ["--loopback", "--port", $"{myPort}", "--peer", $"[::1]:{theirPort}",
                 "--keys", Path.Combine(mine, "keys")], logDirectory: Path.Combine(folder, "engine"));
            var radio = new ScriptedRadio();
            _model = new TableModel(_engine, radio, _desk, _settings);
            _window = new MainWindow(_model, _theme, _settings)
            {
                WindowStartupLocation = WindowStartupLocation.Manual, Left = -30000, Top = -30000,
                ShowActivated = false, ShowInTaskbar = false,
            };
            _view = (MainView)_window.Content;
            _other = new OtherDevice(python, repo, "Other device", theirPort, myPort, theirs);
            _model.StartTheEngine();
            _window.Show();
            try
            {
                Steps(files, big, mine, theirs, radio);
            }
            finally
            {
                _window.Quitting = true;
                _window.Close();
                _engine.Stop();
                _other.Dispose();
            }
            Say(_failed == 0 ? "ALL PASSED" : $"{_failed} FAILED");
            return _failed == 0 ? 0 : 1;
        }

        private void Steps(string[] files, string big, string mine, string theirs, ScriptedRadio radio)
        {
            Say("The window is real, placed off screen, never activated. Presses go through automation peers.");
            var discovered = Until(() => _model.Peers.Count == 1);
            Check(discovered, "start: the other device shows up as a plate");
            if (!discovered)
                throw new InvalidOperationException("The loopback peer was not discovered. " + _other.FailureDetails);
            Check(_model.Presence == "Visible as Driven PC", $"start: the header says \"{_model.Presence}\"");
            var peer = _model.Peers[0];
            Shot("idle");
            foreach (var (_, line) in Stage.Controls(_view)) Say($"      in reach: {line}");

            Say("Holding files");
            _desk.Picked = files[..3];
            Press("Choose files");
            Check(_model.Held.Count == 3, "Choose files -> 3 files in hand (the file dialog itself is Windows' own and was answered by the test)");
            Check(Until(() => _model.Held.All(file => file.Thumbnail is not null)), "each picture shows Explorer's thumbnail");
            Shot("holding");
            Press("Remove stand-in-sea.png");
            Check(_model.Held.Count == 2, "Remove on one file -> 2 files in hand");
            var twin = Path.Combine(Directory.CreateDirectory(Path.Combine(folder, "twin")).FullName, "stand-in-dune.png");
            File.Copy(files[0], twin, overwrite: true);
            _desk.Picked = [Path.GetDirectoryName(files[0])!, twin, files[3]];
            Press("Add files");
            Check(_model.Held.Count == 3 && _model.HasHeldNotice,
                  $"Add with a folder and a same-named file -> only the text file joins, notice: \"{_model.HeldNotice}\"");
            Shot("held-notice");
            Press("Clear the files");
            Check(_model.Held.Count == 0 && Named("Choose files") is not null, "Clear -> the hand is empty again");
            _model.Hold([files[1]]);
            Check(_model.Held.Count == 1, "a drop on the table: its handler's call takes the file in hand (the drag itself needs a pointer and was not made)");
            _model.ClearHeld.Execute(null);

            Say("Sending, accepted at the other end");
            var states = new List<PeerState>();
            string? midway = null;
            peer.PropertyChanged += (_, change) =>
            {
                if (change.PropertyName != nameof(PeerModel.State)) return;
                states.Add(peer.State);
            };
            peer.PropertyChanged += (_, change) =>
            {
                if (change.PropertyName != nameof(PeerModel.Status) || midway is not null) return;
                if (peer.Progress is > 0.2 and < 0.9) midway = Status(peer);
            };
            _desk.Picked = [big, files[0]];
            Press("Choose files");
            Press("Other device");
            var asked = _other.Heard(Trying, "asked");
            Check(asked is not null && peer.State == PeerState.Asking, $"press the plate -> the other device is asked; plate says \"{peer.Status}\"");
            Shot("asking");
            _other.Tell(new { @do = "answer", id = OtherDevice.Text(asked!.Value, "id"), accept = true });
            Until(() => peer.Progress > 0.25, 20);
            Shot("sending");
            Check(Until(() => peer.State == PeerState.Sent, 60), $"accepted there -> states {string.Join(", ", states)}; midway the plate read \"{midway}\"");
            Check(peer.Status == "Sent 2 files" && _model.Held.Count == 0, $"sent -> plate says \"{peer.Status}\" and the hand is empty");
            Shot("sent");
            Check(File.Exists(Path.Combine(theirs, "stand-in-film.mov")) && Hash(Path.Combine(theirs, "stand-in-film.mov")) == Hash(big)
                  && Hash(Path.Combine(theirs, "stand-in-dune.png")) == Hash(files[0]),
                  "both files are at the other end, byte for byte (96 MB and a picture)");
            Check(Until(() => peer.State == PeerState.Idle, 8), "four seconds on -> the plate rests again");

            Say("Sending, declined at the other end");
            _desk.Picked = [files[1]];
            Press("Choose files");
            Press("Other device");
            asked = _other.Heard(Trying, "asked");
            _other.Tell(new { @do = "answer", id = OtherDevice.Text(asked!.Value, "id"), accept = false });
            Check(Until(() => peer.State == PeerState.Declined), $"declined there -> plate says \"{peer.Status}\", file still in hand: {_model.Held.Count}");
            Shot("declined");

            Say("Sending, stopped from here");
            Press("Other device");
            asked = _other.Heard(Trying, "asked");
            Check(peer.State == PeerState.Asking, "press the plate again -> asked again");
            Press("Stop sending to Other device");
            Check(Until(() => peer.State == PeerState.Idle), "Stop -> the plate rests");
            var left = _other.Heard(Trying, "answered", ("id", OtherDevice.Text(asked!.Value, "id")));
            Check(left is not null && OtherDevice.Text(left.Value, "how") == "left", "the other device sees the sender leave");
            Press("Clear the files");

            Say("Receiving, declined here");
            _other.Tell(new { @do = "look", on = true });
            var me = _other.Heard(Trying, "peer");
            Check(me is not null && OtherDevice.Text(me.Value, "name") == "Driven PC", "the other device sees this one by its name");
            var meId = OtherDevice.Text(me!.Value, "id");
            _other.Tell(new { @do = "send", id = "x1", peer = meId, files = new[] { files[1] } });
            Check(Until(() => _model.Incoming.Count == 1 && _model.Incoming[0].IsAsking),
                  $"it sends -> a request card: \"{(_model.Incoming.Count > 0 ? _model.Incoming[0].Headline : "")}\"");
            Check(Named("Choose files")?.Background != _app.Resources["Accent"], "while a request waits, Accept is the only accent button");
            Shot("request");
            Press("Decline");
            Check(Until(() => _model.Incoming.Count == 0), "Decline -> the card goes");
            Check(_other.Heard(Trying, "send", ("id", "x1"), ("state", "declined")) is not null && Directory.GetFiles(mine).Length == 0,
                  "the other device is told no, and nothing was stored");

            Say("Receiving, accepted here");
            _other.Tell(new { @do = "send", id = "x2", peer = meId, files = new[] { big, files[0], files[3] } });
            Check(Until(() => _model.Incoming.Count == 1 && _model.Incoming[0].IsAsking),
                  $"it sends three -> \"{(_model.Incoming.Count > 0 ? _model.Incoming[0].Headline : "")}\", \"{(_model.Incoming.Count > 0 ? _model.Incoming[0].Status : "")}\"");
            Press("Accept");
            string? arriving = null;
            Until(() =>
            {
                if (_model.Incoming.Count > 0 && _model.Incoming[0].Progress is > 0.2 and < 0.95)
                    arriving ??= _model.Incoming[0].Status;
                return arriving is not null || _model.Received.Count > 0;
            }, 30);
            if (_model.Incoming.Count > 0) Shot("receiving");
            Check(Until(() => _model.Received.Count == 3, 60), $"Accept -> three files on the shelf; midway the card read \"{arriving}\"");
            Check(Hash(Path.Combine(mine, "stand-in-film.mov")) == Hash(big) && Hash(Path.Combine(mine, "stand-in-dune.png")) == Hash(files[0]),
                  "they are in the folder, byte for byte");
            Until(() => _model.Received.Any(file => file.Thumbnail is not null), 5);
            Shot("received");
            Press("stand-in-dune.png");
            Check(_desk.Opened.LastOrDefault() == "open " + Path.Combine(mine, "stand-in-dune.png"), "press a received file -> it is opened (recorded, not launched)");
            Press("Open folder");
            Check(_desk.Opened.LastOrDefault() == "open " + mine, "Open folder -> the folder is opened (recorded)");
            _model.Received[0].Reveal!.Execute(null);
            Check(_desk.Opened.Last().StartsWith("reveal "), "Show in folder: the menu item's command runs (the menu was not opened: that takes a right click)");

            Say("Settings");
            Press("Settings");
            Check(_model.SettingsOpen, "Settings -> the sheet opens");
            Check(Stage.Controls(_view).Where(one => one.Line.Contains("\"Radio\"") || one.Line.Contains("\"Other device\""))
                      .All(one => one.Line.Contains("out of reach")), "while it is open, the table behind it cannot be reached");
            Shot("settings");
            foreach (var (_, line) in Stage.Controls(_view).Where(one => one.Control.IsEnabled)) Say($"      in reach: {line}");
            var box = (TextBox)Named("Name other devices see")!;
            ((IValueProvider)UIElementAutomationPeer.CreatePeerForElement(box).GetPattern(PatternInterface.Value)).SetValue("Renamed PC");
            Key(box, System.Windows.Input.Key.Enter);
            Check(Until(() => _model.Name == "Renamed PC"), $"type a name and press Enter -> the engine takes it: \"{_model.Name}\"");
            ((IValueProvider)UIElementAutomationPeer.CreatePeerForElement(box).GetPattern(PatternInterface.Value)).SetValue("bad/name");
            Key(box, System.Windows.Input.Key.Enter);
            Check(Until(() => _model.HasNameError) && _model.Name == "Renamed PC", $"a name with a slash -> \"{_model.NameError}\", the old name stays");
            Shot("settings-refused");
            ((IValueProvider)UIElementAutomationPeer.CreatePeerForElement(box).GetPattern(PatternInterface.Value)).SetValue("Driven PC");
            Key(box, System.Windows.Input.Key.Enter);
            Check(Until(() => _model.Name == "Driven PC" && !_model.HasNameError), "a good name again -> the message goes");
            var elsewhere = Directory.CreateDirectory(Path.Combine(folder, "elsewhere")).FullName;
            _desk.PickedFolder = elsewhere;
            Press("Change folder");
            Check(Until(() => _model.Folder == elsewhere), "Change folder -> received files go there (the folder dialog was answered by the test)");
            _desk.PickedFolder = Path.Combine(folder, "no-such-folder");
            Press("Change folder");
            Check(Until(() => _model.HasFolderError) && _model.Folder == elsewhere, $"a folder that is not there -> \"{_model.FolderError}\"");
            Press("Take everything");
            Check(Until(() => !_model.AskFirst), "Take everything -> chosen");
            _other.Tell(new { @do = "send", id = "x3", peer = meId, files = new[] { files[2] } });
            var prompted = false;
            Check(Until(() =>
            {
                prompted |= _model.Incoming.Any(item => item.IsAsking);
                return File.Exists(Path.Combine(elsewhere, "stand-in-dusk.png"));
            }, 30) && !prompted, "the other device sends -> the file is stored in the new folder with no request shown");
            Press("Ask me first");
            Check(Until(() => _model.AskFirst), "Ask me first -> chosen again");
            Press("Keep receiving when the window is closed");
            Check(!_settings.KeepInTray, "untick Keep receiving -> the setting is off");
            Press("Keep receiving when the window is closed");
            Check(_settings.KeepInTray, "tick it -> on again");
            Press("Project page");
            Check(_desk.Opened.Last() == "open " + TableModel.Project, "Project page -> the repository's address is opened (recorded)");
            Key(Named("Close settings")!, System.Windows.Input.Key.Escape);
            Check(Until(() => !_model.SettingsOpen, 3), "Escape -> the sheet closes");
            Press("Settings");
            var veil = (UIElement)VisualTreeHelper.GetChild((DependencyObject)_view.FindName("Sheet"), 0);
            veil.RaiseEvent(new MouseButtonEventArgs(Mouse.PrimaryDevice, 0, MouseButton.Left)
            {
                RoutedEvent = UIElement.MouseLeftButtonDownEvent,
            });
            Check(Until(() => !_model.SettingsOpen, 3), "a press beside the sheet -> it closes");
            Press("Settings");
            Press("Close settings");
            Check(!_model.SettingsOpen && Named("Radio") is not null, "its Close button -> closed, and the table is in reach again");

            Say("The radio switch (the radio here is a stand-in: starting the real one asks Windows for administrator rights)");
            Press("Radio");
            Check(_model.Notices.Any(notice => notice.Contains("terminal")) && radio.Starts == 0,
                  $"switch off while the link was not started by the app -> \"{_model.Notices.FirstOrDefault()}\"");
            Shot("radio-notice");

            Say("Themes and sizes");
            _theme.Apply(Palette.Light(ThemeService.SystemAccent()));
            Pump(150);
            Shot("light");
            _theme.Apply(Palette.System(Rgb.Black, Rgb.White, Rgb.Hex("#1AEBFF"), Rgb.Black, Rgb.Hex("#FFFF00")));
            Pump(150);
            Shot("contrast-theme");
            _theme.Apply(Palette.Night(ThemeService.SystemAccent()));
            _desk.Picked = files;
            Press("Choose files");
            foreach (var (width, height) in new[] { (380.0, 560.0), (900.0, 820.0), (460.0, 720.0) })
            {
                (_window.Width, _window.Height) = (width, height);
                Pump(200);
                var wide = Overflowing();
                Check(wide.Count == 0, $"at {width} by {height} nothing reaches past the window's sides {string.Join("; ", wide.Take(3))}");
                Shot($"size-{width}");
            }

            Say("Text size, as set under Accessibility in Windows (set here on the window; the setting itself was left alone)");
            _window.TextScale = 1.5;
            (_window.Width, _window.Height) = (600, 860);
            Pump(250);
            var cut = Overflowing();
            Check(cut.Count == 0, $"at 150% text in a 600 by 860 window nothing reaches past the sides {string.Join("; ", cut.Take(3))}");
            Shot("text-150");
            _window.TextScale = 1;
            (_window.Width, _window.Height) = (460, 720);
            Pump(200);

            Say("Under the pointer and pressed (the states are set directly: moving the real pointer was not done)");
            var plate = (Control)Named("Other device")!;
            var surface = (Raised)plate.Template.FindName("Surface", plate);
            var rest = Shot("plate-at-rest");
            surface.IsHot = true;
            Pump(150);
            var hot = Shot("plate-under-pointer");
            surface.IsDown = true;
            Pump(150);
            var down = Shot("plate-pressed");
            surface.ClearValue(Raised.IsHotProperty);
            surface.ClearValue(Raised.IsDownProperty);
            Check(Hash(rest) != Hash(hot) && Hash(hot) != Hash(down), "a plate rises under the pointer and sinks when pressed: the three pictures differ");

            Say("The window as Windows draws it, title bar included");
            var whole = Path.Combine(folder, "window-as-drawn.png");
            Check(Screen.Print(_window, whole), $"the whole window is printed to {Path.GetFileName(whole)}");
            Check(_window.Icon is null && System.Drawing.Icon.ExtractAssociatedIcon(Environment.ProcessPath!) is not null,
                  "the window takes the icon built into the app, which is the one Explorer shows for it");

            Say("Focus rings (drawn from each control's own focus style; Tab itself was not pressed)");
            var layer = AdornerLayer.GetAdornerLayer(_view)!;
            var ringed = new[] { "Radio", "Settings", "Other device", "Add files" }.Select(Named).OfType<Control>().ToList();
            var rings = ringed.Select(control => new RingAdorner(control)).ToList();
            foreach (var ring in rings) layer.Add(ring);
            Pump(120);
            Shot("focus-rings");
            foreach (var ring in rings) layer.Remove(ring);
            var missing = Stage.Controls(_view).Where(one => one.Line.Contains("NO FOCUS RING") || one.Line.Contains("not a tab stop")).ToList();
            Check(missing.Count == 0, $"every control in reach is a tab stop with a focus ring {string.Join("; ", missing.Select(one => one.Line))}");
            Press("Clear the files");

            Say("Motion");
            Motion.Reduced = false;
            _desk.Picked = files[..3];
            Press("Choose files");
            var frames = new List<string>();
            for (var frame = 0; frame < 6; frame++)
            {
                frames.Add(Shot($"drop-frame-{frame}"));
                Pump(50);
            }
            Pump(600);
            frames.Add(Shot("drop-rest"));
            Check(Hash(frames[0]) != Hash(frames[^1]), "dropped files move before they rest: the first frame differs from the last");
            Stage.Sheet(frames, Path.Combine(folder, "motion-drop.png"), 360);
            Motion.Reduced = true;
            Press("Clear the files");
            Press("Choose files");
            Check(Hash(Shot("drop-reduced-first")) == Hash(Shot("drop-reduced-later")),
                  "with animation effects off in Windows, they are at rest at once");
            Press("Clear the files");

            Say("The engine ending by itself");
            Process.GetProcessById(_engine.ProcessId!.Value).Kill(entireProcessTree: true);
            Check(Until(() => _model.Across == Across.EngineDown), $"the engine is killed -> \"{_model.PanelTitle}\": \"{_model.PanelBody}\"");
            Shot("engine-stopped");
            Press("Start it again");
            Check(Until(() => _model.Across == Across.Nearby && _model.Peers.Count == 1, 30), "Start it again -> the engine runs and the device is back");

            Say("The notification area icon");
            using (new Tray(_window, () => { }))
                Pump(300);
            Check(true, "the icon is created and removed without an error (its menu and its notice were not shown)");
        }
    }
}
