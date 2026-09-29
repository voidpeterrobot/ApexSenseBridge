using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;

namespace ApexSenseBridgeTray.Services
{
    // Keep readiness, launch and rollback ordering independently testable without hardware.
    public static class PreparedGameLaunch
    {
        public static Process StartGame(string executablePath, Func<ProcessStartInfo, Process> start)
        {
            var info = new ProcessStartInfo(executablePath) {
                WorkingDirectory = Path.GetDirectoryName(executablePath), UseShellExecute = false
            };
            try { return start(info); }
            catch (Win32Exception ex) {
                // ERROR_ELEVATION_REQUIRED means CreateProcess did not start the game.
                // Elevate only that executable, keeping Tray and its settings under the user.
                if (ex.NativeErrorCode != 740) throw;
            }
            info.UseShellExecute = true;
            info.Verb = "runas";
            try { return start(info); }
            catch (Win32Exception ex) {
                if (ex.NativeErrorCode == 1223)
                    throw new InvalidOperationException("Administrator approval was cancelled. The game was not launched and the prepared bridge will stop.", ex);
                throw;
            }
        }

        public static void Run(Action prepare, Func<bool> isHealthy, Action launch, Action rollback)
        {
            prepare(); // A failed preparation owns its own cleanup; never stop somebody else's session.
            try {
                if (!isHealthy()) throw new InvalidOperationException("The bridge stopped before the game could launch.");
                launch();
            }
            catch {
                rollback();
                throw;
            }
        }
    }
}
