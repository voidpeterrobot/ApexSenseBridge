using ApexSenseBridgeTray.Models;

namespace ApexSenseBridgeTray.Services
{
    public static class GameActivationPolicy
    {
        public static bool IsPassiveManualMode(string forcedProfile, bool activeApex6)
        {
            return !activeApex6 && !string.IsNullOrWhiteSpace(forcedProfile) &&
                !string.Equals(forcedProfile, "none", System.StringComparison.OrdinalIgnoreCase);
        }
        public static bool ShouldActivate(
            SupportedGame game,
            TraySettings settings,
            string executableTitle,
            string folderName,
            string fileName)
        {
            if (game == null || settings == null) return false;

            if (settings.IsGameExcluded(game.Normalized) ||
                settings.IsGameExcluded(game.Title) ||
                settings.IsGameExcluded(executableTitle) ||
                settings.IsGameExcluded(folderName) ||
                settings.IsGameExcluded(fileName) ||
                (game.SteamAppIdVerified && game.SteamAppId > 0 &&
                 settings.IsGameExcluded(game.SteamAppId.ToString())))
            {
                return false;
            }

            return (settings.TriggerOnAdaptiveTriggers && game.AdaptiveTriggers) ||
                   (settings.TriggerOnHapticFeedback && game.HapticFeedback);
        }
    }
}
