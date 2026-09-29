using System;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Text;
using System.Threading;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using ApexSenseBridge.Shared;
class Apex6ManagedTests
{
    static void Check(bool ok, string why) { if (!ok) throw new Exception(why); }
    static void Reject(Action action) { bool rejected = false; try { action(); } catch { rejected = true; } Check(rejected, "expected rejection"); }
    [STAThread]
    static int Main()
    {
        try {
            var root = Path.Combine(Path.GetTempPath(), "asb-apex6-managed-" + Guid.NewGuid().ToString("N"));
            var path = Path.Combine(root, "settings.txt");
            var defaults = Apex6Beta.ReadUnlocked(path); Check(defaults.ConsentVersion == 0 && defaults.Gain == 1, "migration defaults");
            var a = new Thread(() => { for (int i=0; i<30; ++i) Apex6Beta.UpdateFile(path, true, null); });
            var b = new Thread(() => { for (int i=0; i<30; ++i) Apex6Beta.UpdateFile(path, null, 3.5); });
            a.Start(); b.Start(); a.Join(); b.Join();
            var saved = Apex6Beta.ReadUnlocked(path); Check(saved.ConsentVersion == 1 && saved.Gain == 3.5, "shared concurrent updates");
            Apex6Beta.UpdateFile(path, false, null); Check(Apex6Beta.ReadUnlocked(path).Gain == 3.5, "consent update reset gain");
            Reject(() => Apex6Beta.UpdateFile(path, null, double.NaN));
            Reject(() => Apex6Beta.UpdateFile(path, null, double.PositiveInfinity));
            Reject(() => Apex6Beta.UpdateFile(path, null, 12.5));
            File.WriteAllText(path, "ASB_APEX6_SETTINGS_V1\n9\n1\n"); Reject(() => Apex6Beta.ReadUnlocked(path));
            var token = Guid.NewGuid().ToString("N"); var name = @"Local\ApexSenseBridge.Grip.v1." + token;
            using (var mutex = new Mutex(false, name + ".Lock"))
            using (var map = MemoryMappedFile.CreateNew(name, 56))
            using (var view = map.CreateViewAccessor(0, 56)) {
                view.Write(0, 0x47425341u); view.Write(4, 1u); var bytes = Encoding.ASCII.GetBytes(token); view.WriteArray(8, bytes, 0, 32);
                Apex6Beta.SendGain(token, 0); Check(view.ReadUInt32(40) == 1 && view.ReadDouble(48) == 0, "mute IPC");
                Apex6Beta.SendGain(token, 1); Check(view.ReadUInt32(40) == 2 && view.ReadDouble(48) == 1, "reset IPC");
                view.Write(8, (byte)'X'); Reject(() => Apex6Beta.SendGain(token, 2));
            }
            Reject(() => Apex6Beta.SendGain(token, 2));
            TestEmbeddedControls();
            Directory.Delete(root, true);
            Console.WriteLine("Managed beta migration, atomic updates, gain/mute/reset, stale-session IPC and embedded controls passed."); return 0;
        } catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }
    static void TestEmbeddedControls()
    {
        var preferences = new Apex6Preferences { ConsentVersion = 1, Gain = 2 };
        double sent = -1; int sends = 0;
        var panel = (StackPanel)Apex6Beta.CreateControls(gain => { sent = gain; sends++; }, () => "HID rumble",
            () => preferences, (consent, gain) => {
                if (consent.HasValue) preferences.ConsentVersion = consent.Value ? 1 : 0;
                if (gain.HasValue) preferences.Gain = gain.Value;
            });
        panel.RaiseEvent(new RoutedEventArgs(FrameworkElement.LoadedEvent));
        var slider = panel.Children.OfType<Slider>().Single();
        Check(slider.Value == 2 && sends == 0, "loading shared settings must not send an actuator command");
        Check(panel.Children.OfType<TextBlock>().Any(t => t.Text == "HID rumble"), "status is displayed inline");
        slider.Value = 2.5;
        Check(sent == 2.5 && preferences.Gain == 2.5, "embedded slider updates live and persisted gain");
        var buttons = panel.Children.OfType<StackPanel>().Single();
        buttons.Children.OfType<Button>().Single(b => (string)b.Content == "Mute").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Check(sent == 0 && slider.Value == 0 && preferences.Gain == 0, "embedded mute persists and sends zero");
        buttons.Children.OfType<Button>().Single(b => (string)b.Content == "Reset to 1").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        Check(sent == 1 && slider.Value == 1 && preferences.Gain == 1, "embedded reset restores gain one");
        int previous = sends;
        preferences.Gain = 4;
        panel.RaiseEvent(new RoutedEventArgs(FrameworkElement.LoadedEvent));
        Check(slider.Value == 4 && sends == previous, "shared preference refresh does not echo a live command");
        panel.RaiseEvent(new RoutedEventArgs(FrameworkElement.UnloadedEvent));
    }
}
