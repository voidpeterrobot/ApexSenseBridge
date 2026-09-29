using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;

namespace ApexSenseBridge.Shared
{
    public sealed class Apex6Preferences
    {
        public int ConsentVersion;
        public double Gain = 1;
    }

    public static class Apex6Beta
    {
        public const int ConsentVersion = 1;
        public static int? ControllerIndex { get; private set; }
        private static readonly string SettingsPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "ApexSenseBridge", "apex6-settings-v1.txt");
        private static bool ValidGain(double gain) { return !double.IsNaN(gain) && !double.IsInfinity(gain) && gain >= 0 && gain <= 12; }
        private static T Locked<T>(Func<T> work)
        {
            using (var mutex = new Mutex(false, @"Local\ApexSenseBridge.Apex6Settings.v1"))
            {
                bool held;
                try { held = mutex.WaitOne(5000); } catch (AbandonedMutexException) { held = true; }
                if (!held) throw new IOException("Apex6 settings are busy.");
                try { return work(); } finally { mutex.ReleaseMutex(); }
            }
        }
        internal static Apex6Preferences ReadUnlocked(string path)
        {
            if (!File.Exists(path)) return new Apex6Preferences();
            if (new FileInfo(path).Length > 1024) throw new IOException("Invalid Apex6 settings size.");
            var parts = File.ReadAllText(path).Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
            int consent; double gain;
            if (parts.Length != 3 || parts[0] != "ASB_APEX6_SETTINGS_V1" ||
                !int.TryParse(parts[1], out consent) || consent < 0 || consent > ConsentVersion ||
                !double.TryParse(parts[2], NumberStyles.Float, CultureInfo.InvariantCulture, out gain) || !ValidGain(gain))
                throw new IOException("Invalid Apex6 settings; beta remains disabled.");
            return new Apex6Preferences { ConsentVersion = consent, Gain = gain };
        }
        public static Apex6Preferences Read() { return Locked(() => ReadUnlocked(SettingsPath)); }
        public static void Update(bool? consent, double? gain)
        { UpdateFile(SettingsPath, consent, gain); }
        internal static void UpdateFile(string path, bool? consent, double? gain)
        {
            if (gain.HasValue && !ValidGain(gain.Value)) throw new ArgumentOutOfRangeException("gain");
            Locked(() => {
                var settings = ReadUnlocked(path);
                if (consent.HasValue) settings.ConsentVersion = consent.Value ? ConsentVersion : 0;
                if (gain.HasValue) settings.Gain = gain.Value;
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
                try {
                    File.WriteAllText(temporary, "ASB_APEX6_SETTINGS_V1\n" + settings.ConsentVersion + "\n" + settings.Gain.ToString("R", CultureInfo.InvariantCulture) + "\n", new UTF8Encoding(false));
                    if (File.Exists(path)) File.Replace(temporary, path, null);
                    else File.Move(temporary, path);
                } finally { if (File.Exists(temporary)) File.Delete(temporary); }
                return true;
            });
        }
        public static void SendGain(string token, double gain)
        {
            if (token == null || !Regex.IsMatch(token, "\\A[0-9a-f]{32}\\z") || !ValidGain(gain)) throw new ArgumentException("Invalid grip control command.");
            var name = @"Local\ApexSenseBridge.Grip.v1." + token;
            using (var mutex = Mutex.OpenExisting(name + ".Lock"))
            using (var mapping = MemoryMappedFile.OpenExisting(name, MemoryMappedFileRights.ReadWrite))
            using (var view = mapping.CreateViewAccessor(0, 56))
            {
                if (!mutex.WaitOne(1000)) throw new IOException("Grip control is busy.");
                try {
                    var bytes = new byte[32]; view.ReadArray(8, bytes, 0, 32);
                    if (view.ReadUInt32(0) != 0x47425341 || view.ReadUInt32(4) != 1 || Encoding.ASCII.GetString(bytes) != token)
                        throw new IOException("Stale or incompatible grip session.");
                    var revision = checked(view.ReadUInt32(40) + 1);
                    view.Write(48, gain); view.Write(40, revision); view.Flush();
                } finally { mutex.ReleaseMutex(); }
            }
        }
        public static bool SelectedControllerIsApex6(string engine)
        {
            using (var process = Process.Start(new ProcessStartInfo(engine, "list") {
                UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true }))
            {
                var read = process.StandardOutput.ReadToEndAsync();
                var errors = process.StandardError.ReadToEndAsync();
                if (!process.WaitForExit(5000)) { process.Kill(); throw new IOException("Controller discovery timed out."); }
                var text = read.Result;
                if (process.ExitCode != 0) throw new IOException("Controller discovery failed: " + errors.Result + text);
                var devices = Regex.Matches(text, @"(?m)^\[(\d+)\]");
                if (!ControllerIndex.HasValue && devices.Count != 1)
                    throw new IOException("Select a controller in the control center using the index shown by ApexSenseBridge list.\n" + text);
                var index = ControllerIndex ?? 0;
                if (index >= devices.Count) throw new IOException("Controller index is no longer available. Refresh controller selection.");
                return Regex.IsMatch(text, @"Apex6 Pro USB beta:[^\r\n]*\r?\n\[" + index + @"\]");
            }
        }
        public static string SelectionArgument { get { return ControllerIndex.HasValue ? " " + ControllerIndex.Value.ToString(CultureInfo.InvariantCulture) : ""; } }

        // Embedded by both hosts. No separate modal window and no timer while unloaded/hidden.
        public static FrameworkElement CreateControls(Action<double> liveGain, Func<string> readStatus = null)
        {
            return CreateControls(liveGain, readStatus, Read, Update);
        }

        internal static FrameworkElement CreateControls(Action<double> liveGain, Func<string> readStatus,
            Func<Apex6Preferences> readPreferences, Action<bool?, double?> updatePreferences)
        {
            var panel = new StackPanel();
            panel.Children.Add(new TextBlock { Text = "Apex6 Pro · USB grip beta", FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 0, 0, 8) });
            panel.Children.Add(new TextBlock { Text = "Grip feedback over direct USB. Adaptive triggers, wireless and onboard profiles are unavailable. Close Flydigi Space Station and stop its service before starting.", TextWrapping = TextWrapping.Wrap, Opacity = .8 });
            var consent = new CheckBox { Content = "Enable Apex6 Pro grip beta", Margin = new Thickness(0, 12, 0, 12) };
            consent.SetBinding(Control.ForegroundProperty, new Binding { Source = panel, Path = new PropertyPath(TextBlock.ForegroundProperty) });
            panel.Children.Add(consent);
            var error = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 6, 0, 6) };
            consent.Click += (s,e) => { try { updatePreferences(consent.IsChecked == true, null); error.Text = "Consent applies to the next session. Stop the bridge to end the current session."; } catch (Exception ex) { error.Text = ex.Message; } };
            var advanced = new Expander { Header = "Controller selection", Margin = new Thickness(0, 0, 0, 8) };
            advanced.SetBinding(Control.ForegroundProperty, new Binding { Source = panel, Path = new PropertyPath(TextBlock.ForegroundProperty) });
            var selection = new StackPanel(); advanced.Content = selection; panel.Children.Add(advanced);
            selection.Children.Add(new TextBlock { Text = "Controller index (blank selects the only controller; see CLI list):", TextWrapping = TextWrapping.Wrap });
            var index = new TextBox { Text = ControllerIndex.HasValue ? ControllerIndex.ToString() : "", Margin = new Thickness(0, 4, 0, 10) }; selection.Children.Add(index);
            index.LostFocus += (s,e) => { int value; if (string.IsNullOrWhiteSpace(index.Text)) ControllerIndex = null; else if (int.TryParse(index.Text, out value) && value >= 0) ControllerIndex = value; else error.Text = "Enter a nonnegative controller index."; };
            var label = new TextBlock(); panel.Children.Add(label);
            var status = new TextBlock { TextWrapping = TextWrapping.Wrap }; panel.Children.Add(status);
            bool refreshing = false;
            var slider = new Slider { Minimum = 0, Maximum = 12, TickFrequency = .5, SmallChange = .5, LargeChange = .5, IsSnapToTickEnabled = true, Value = 1, Margin = new Thickness(0, 8, 0, 8), ToolTip = "Global grip strength · 0 to 12" }; panel.Children.Add(slider);
            Action<double> change = gain => { try { updatePreferences(null, gain); label.Text = "Global grip gain: " + gain.ToString("0.0", CultureInfo.InvariantCulture); if (liveGain != null) liveGain(gain); error.Text = ""; } catch (Exception ex) { error.Text = ex.Message; } };
            slider.ValueChanged += (s,e) => { if (!refreshing) change(slider.Value); };
            var buttons = new StackPanel { Orientation = Orientation.Horizontal }; panel.Children.Add(buttons);
            Action<double> setGain = gain => { refreshing = true; slider.Value = gain; refreshing = false; change(gain); };
            var mute = new Button { Content = "Mute", Margin = new Thickness(0, 0, 10, 0), Padding = new Thickness(12, 4, 12, 4) }; buttons.Children.Add(mute); mute.Click += (s,e) => setGain(0);
            var reset = new Button { Content = "Reset to 1", Padding = new Thickness(12, 4, 12, 4) }; buttons.Children.Add(reset); reset.Click += (s,e) => setGain(1);
            panel.Children.Add(new TextBlock { Text = "HID rumble sliders work without audio setup. PCM haptics require four-channel 48 kHz audio routed to the virtual DualSense.", TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 12, 0, 0) });
            panel.Children.Add(error);
            Action refresh = () => {
                try {
                    status.Text = readStatus == null ? "No Apex6 session" : readStatus();
                    var preferences = readPreferences();
                    refreshing = true;
                    consent.IsChecked = preferences.ConsentVersion == ConsentVersion;
                    if (!slider.IsMouseCaptureWithin) slider.Value = preferences.Gain;
                    label.Text = "Global grip gain: " + slider.Value.ToString("0.0", CultureInfo.InvariantCulture);
                } catch (Exception ex) { error.Text = ex.Message; }
                finally { refreshing = false; }
            };
            var timer = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
            timer.Tick += (s,e) => refresh();
            panel.Loaded += (s,e) => {
                // Tray supplies these styles; Playnite keeps its host's native styles.
                var buttonStyle = panel.TryFindResource("BtnSecondary") as Style;
                if (buttonStyle != null) { mute.Style = buttonStyle; reset.Style = buttonStyle; }
                var checkboxStyle = panel.TryFindResource("PsCheckBox") as Style;
                if (checkboxStyle != null) consent.Style = checkboxStyle;
                refresh(); if (panel.IsVisible) timer.Start();
            };
            panel.Unloaded += (s,e) => timer.Stop();
            panel.IsVisibleChanged += (s,e) => { if (panel.IsVisible && panel.IsLoaded) { refresh(); timer.Start(); } else timer.Stop(); };
            return panel;
        }
    }
}
