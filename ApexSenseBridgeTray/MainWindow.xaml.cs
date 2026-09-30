using ApexSenseBridgeTray.Common;
using ApexSenseBridgeTray.Models;
using ApexSenseBridgeTray.Services;
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace ApexSenseBridgeTray
{
    public partial class MainWindow : Window
    {
        private readonly CloudGameListService gameListService;
        private readonly EngineSessionManager sessionManager;
        private readonly ExecutableLearningService learningService;
        private readonly ProcessMonitorService monitorService;
        private readonly UpdateCheckerService updateChecker;
        private readonly TraySettings settings;
        private bool isInitialized;
        private UpdateInfo latestUpdateInfo;
        private bool launchingGame;

        public MainWindow(
            CloudGameListService gameListService,
            EngineSessionManager sessionManager,
            ExecutableLearningService learningService,
            ProcessMonitorService monitorService,
            UpdateCheckerService updateChecker,
            TraySettings settings)
        {
            this.gameListService = gameListService;
            this.sessionManager = sessionManager;
            this.learningService = learningService;
            this.monitorService = monitorService;
            this.updateChecker = updateChecker;
            this.settings = settings;

            InitializeComponent();
            GripControlsHost.Content = global::ApexSenseBridge.Shared.Apex6Beta.CreateControls(
                sessionManager.SetGripGain, () => sessionManager.GripStatus, true);
            ChkApex6Dongle.IsChecked = settings.Apex6DongleBeta;
            RefreshLaunchWhitelist();

            UpdateLanguageRadios();

            ChkAutoDetect.IsChecked = settings.AutoDetectGames;
            ChkTriggerAdaptive.IsChecked = settings.TriggerOnAdaptiveTriggers;
            ChkTriggerHaptic.IsChecked = settings.TriggerOnHapticFeedback;
            PnlTriggerCriteria.IsEnabled = settings.AutoDetectGames;
            PnlTriggerCriteria.Opacity = settings.AutoDetectGames ? 1.0 : 0.4;
            ChkNotifications.IsChecked = settings.EnableNotifications;
            ChkManualBridge.IsChecked = settings.ForcedProfile == "standard";

            if (updateChecker != null)
            {
                TxtVersionInfo.Text = string.Format("ApexSenseBridge v{0}", updateChecker.GetCurrentVersion());
                updateChecker.UpdateAvailable += (info) => Dispatcher.BeginInvoke(new Action(() => ShowUpdateBanner(info)));
            }

            UpdateDatabaseCount();
            UpdateSessionStatus();

            isInitialized = true;

            sessionManager.SessionStarted += (game, profile) => Dispatcher.BeginInvoke(new Action(() =>
            {
                try { UpdateSessionStatus(); } catch { }
            }));
            sessionManager.SessionStopped += (reason) => Dispatcher.BeginInvoke(new Action(() =>
            {
                if (!sessionManager.IsSessionActive && sessionManager.LastSessionWasApex6) {
                    settings.ForcedProfile = "none";
                    isInitialized = false;
                    ChkManualBridge.IsChecked = false;
                    isInitialized = true;
                    settings.Save();
                }
                try { UpdateSessionStatus(); } catch { }
            }));
            sessionManager.SessionError += (err) => Dispatcher.BeginInvoke(new Action(() =>
            {
                try
                {
                    UpdateSessionStatus();
                }
                catch { }
            }));
            gameListService.GamesUpdated += () => Dispatcher.BeginInvoke(new Action(() =>
            {
                try { UpdateDatabaseCount(); } catch { }
            }));
            if (learningService != null)
            {
                learningService.BindingsChanged += () => Dispatcher.BeginInvoke(new Action(() =>
                {
                    try { UpdateDatabaseCount(); } catch { }
                }));
            }
            ThemeManager.ThemeChanged += () => Dispatcher.BeginInvoke(new Action(() =>
            {
                try { UpdateSessionStatus(); } catch { }
            }));
            LocalizationManager.LanguageChanged += () => Dispatcher.BeginInvoke(new Action(() =>
            {
                try
                {
                    UpdateLanguageRadios();
                    UpdateSessionStatus();
                    UpdateDatabaseCount();
                    if (latestUpdateInfo != null)
                    {
                        ShowUpdateBanner(latestUpdateInfo);
                    }
                }
                catch { }
            }));
        }

        private void UpdateLanguageRadios()
        {
            bool isFr = LocalizationManager.CurrentLanguage == LocalizationManager.LangFrench;
            RadLangFr.IsChecked = isFr;
            RadLangEn.IsChecked = !isFr;
        }
        private void RefreshLaunchWhitelist()
        {
            LstLaunchWhitelist.ItemsSource = settings.GetLaunchWhitelist();
        }

        private void OnWhitelistSelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (BtnLaunchWhitelistedGame == null) return;
            BtnLaunchWhitelistedGame.IsEnabled = !launchingGame && LstLaunchWhitelist.SelectedItem != null && !sessionManager.IsSessionActive;
            BtnRemoveWhitelistedGame.IsEnabled = !launchingGame && LstLaunchWhitelist.SelectedItem != null;
        }

        private void OnAddWhitelistedGame(object sender, RoutedEventArgs e)
        {
            var picker = new Microsoft.Win32.OpenFileDialog { Title = "Add a game to the whitelist", Filter = "Game executable (*.exe)|*.exe", CheckFileExists = true };
            if (picker.ShowDialog(this) != true) return;
            try {
                settings.SetLaunchWhitelisted(picker.FileName, true); settings.Save();
                RefreshLaunchWhitelist(); LstLaunchWhitelist.SelectedItem = picker.FileName;
            } catch (Exception ex) { TxtLaunchStatus.Text = ex.Message; }
        }

        private void OnRemoveWhitelistedGame(object sender, RoutedEventArgs e)
        {
            var path = LstLaunchWhitelist.SelectedItem as string;
            if (path == null) return;
            settings.SetLaunchWhitelisted(path, false); settings.Save(); RefreshLaunchWhitelist();
        }

        private async void OnLaunchWhitelistedGame(object sender, RoutedEventArgs e)
        {
            var path = LstLaunchWhitelist.SelectedItem as string;
            if (path == null || launchingGame) return;
            launchingGame = true;
            ChkApex6Dongle.IsEnabled = false;
            BtnAddWhitelistedGame.IsEnabled = false;
            ChkManualBridge.IsEnabled = false;
            BtnStopBridge.IsEnabled = false;
            OnWhitelistSelectionChanged(null, null);
            TxtLaunchStatus.Text = "Preparing controller… The game will start after the bridge is ready.";
            try {
                await Task.Run(() => monitorService.LaunchWhitelistedGame(path));
                TxtLaunchStatus.Text = "Game launched. The bridge will stop when the game exits.";
            } catch (Exception ex) { TxtLaunchStatus.Text = ex.Message; }
            finally {
                launchingGame = false;
                BtnAddWhitelistedGame.IsEnabled = true;
                ChkManualBridge.IsEnabled = true;
                UpdateSessionStatus();
            }
        }

        private async void OnStopBridge(object sender, RoutedEventArgs e)
        {
            BtnStopBridge.IsEnabled = false;
            await Task.Run(() => sessionManager.StopSession("Stopped from control center"));
            UpdateSessionStatus();
        }

        private void OnApex6DongleChanged(object sender, RoutedEventArgs e)
        {
            if (!isInitialized) return;
            settings.Apex6DongleBeta = ChkApex6Dongle.IsChecked == true;
            settings.Save();
        }

        private void OnLanguageOptionChecked(object sender, RoutedEventArgs e)
        {
            if (!isInitialized) return;
            string newLang = RadLangFr.IsChecked == true ? LocalizationManager.LangFrench : LocalizationManager.LangEnglish;
            if (newLang != LocalizationManager.CurrentLanguage)
            {
                settings.Language = newLang;
                settings.Save();
                LocalizationManager.SetLanguage(newLang);
            }
        }

        private void OnWindowDrag(object sender, MouseButtonEventArgs e)
        {
            if (e.ChangedButton == MouseButton.Left)
            {
                DragMove();
            }
        }

        protected override void OnClosing(CancelEventArgs e)
        {
            e.Cancel = true;
            Hide();
        }

        public void UpdateSessionStatus()
        {
            OnWhitelistSelectionChanged(null, null);
            BtnStopBridge.IsEnabled = sessionManager.IsSessionActive && !launchingGame;
            ChkApex6Dongle.IsEnabled = !sessionManager.IsSessionActive && !launchingGame;
            PillTriggers.Visibility = sessionManager.HasActiveApex6Session ? Visibility.Collapsed : Visibility.Visible;
            if (sessionManager.IsSessionActive)
            {
                BadgeStatus.SetResourceReference(Border.BackgroundProperty, "BadgeActiveBg");
                TxtStatusBadge.SetResourceReference(TextBlock.ForegroundProperty, "BadgeActiveFg");
                TxtStatusBadge.Text = LocalizationManager.Get("Loc_StatusBadgeActive");

                TxtActiveGame.Text = sessionManager.ActiveGameTitle;
                TxtActiveGame.FontSize = 16;

                string prof = sessionManager.ActiveProfile;
                if (string.Equals(prof, "standard", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(prof, "none", StringComparison.OrdinalIgnoreCase) ||
                    string.IsNullOrWhiteSpace(prof))
                {
                    TxtActiveProfile.Text = LocalizationManager.Get("Loc_ProfileStandard");
                }
                else
                {
                    TxtActiveProfile.Text = LocalizationManager.Get("Loc_ProfileRemapping");
                }

                TxtWaitHint.Visibility = Visibility.Collapsed;

                PillTriggers.Opacity = 1.0;
                PillHaptics.Opacity = 1.0;

                string manualTitle = LocalizationManager.Get("Loc_ManualBridgeGameTitle");
                bool isForced = sessionManager.ActiveGameTitle == "Pont manuel forcé" ||
                                sessionManager.ActiveGameTitle == "Forced manual bridge" ||
                                sessionManager.ActiveGameTitle == manualTitle;
                BtnExcludeCurrentGame.Visibility = isForced ? Visibility.Collapsed : Visibility.Visible;
            }
            else
            {
                BadgeStatus.SetResourceReference(Border.BackgroundProperty, "BadgeStandbyBg");
                TxtStatusBadge.SetResourceReference(TextBlock.ForegroundProperty, "BadgeStandbyFg");
                TxtStatusBadge.Text = LocalizationManager.Get("Loc_StatusBadgeStandby");

                TxtActiveGame.Text = LocalizationManager.Get("Loc_NoActiveGame");
                TxtActiveGame.FontSize = 15;
                TxtActiveProfile.Text = LocalizationManager.Get("Loc_ProfileStandard");
                TxtWaitHint.Visibility = Visibility.Visible;

                PillTriggers.Opacity = 0.4;
                PillHaptics.Opacity = 0.4;

                BtnExcludeCurrentGame.Visibility = Visibility.Collapsed;
            }
        }

        private void OnExcludeCurrentGameClick(object sender, RoutedEventArgs e)
        {
            var gameTitle = sessionManager.ActiveGameTitle;
            if (string.IsNullOrWhiteSpace(gameTitle) || gameTitle == "Aucun" || gameTitle == "None" ||
                gameTitle == "Pont manuel forcé" || gameTitle == "Forced manual bridge") return;

            SupportedGame game;
            if (gameListService.TryFindGame(gameTitle, out game) && game != null)
            {
                settings.SetGameExcluded(game.Normalized, true);
                settings.SetGameExcluded(game.Title, true);
            }
            else
            {
                settings.SetGameExcluded(gameTitle, true);
            }
            settings.Save();

            sessionManager.StopSession("Game excluded by user");
            UpdateSessionStatus();

            MessageBox.Show(this, LocalizationManager.Format("Loc_MsgExcluded", gameTitle),
                            LocalizationManager.Get("Loc_AppName"), MessageBoxButton.OK, MessageBoxImage.Information);
        }

        public void UpdateDatabaseCount()
        {
            int count = gameListService != null ? gameListService.TotalGamesLoaded : 0;
            string key = count > 1 ? "Loc_CertifiedGamesPlural" : "Loc_CertifiedGamesSingular";
            string baseStr = LocalizationManager.Format(key, count);

            int learned = learningService != null ? learningService.Count : 0;
            if (learned > 0)
            {
                string learnedKey = learned > 1 ? "Loc_LearnedCountPlural" : "Loc_LearnedCountSingular";
                TxtDatabaseInfo.Text = string.Format("{0} • {1}", baseStr, LocalizationManager.Format(learnedKey, learned));
            }
            else
            {
                TxtDatabaseInfo.Text = baseStr;
            }
        }

        private void OnAutoDetectChanged(object sender, RoutedEventArgs e)
        {
            if (!isInitialized) return;
            settings.AutoDetectGames = ChkAutoDetect.IsChecked == true;
            PnlTriggerCriteria.IsEnabled = settings.AutoDetectGames;
            PnlTriggerCriteria.Opacity = settings.AutoDetectGames ? 1.0 : 0.4;
            settings.Save();

            if (settings.AutoDetectGames)
            {
                monitorService.ForceCheck();
            }
            else if (sessionManager.IsSessionActive && settings.ForcedProfile == "none")
            {
                sessionManager.StopSession("Auto-detect disabled by user");
            }
        }

        private void OnTriggerCriteriaChanged(object sender, RoutedEventArgs e)
        {
            if (!isInitialized) return;
            settings.TriggerOnAdaptiveTriggers = ChkTriggerAdaptive.IsChecked == true;
            settings.TriggerOnHapticFeedback = ChkTriggerHaptic.IsChecked == true;
            settings.Save();
            if (settings.AutoDetectGames)
            {
                monitorService.ForceCheck();
            }
        }

        private void OnNotificationsChanged(object sender, RoutedEventArgs e)
        {
            if (!isInitialized) return;
            settings.EnableNotifications = ChkNotifications.IsChecked == true;
            settings.Save();
        }

        private void OnManualBridgeChanged(object sender, RoutedEventArgs e)
        {
            if (!isInitialized) return;
            bool isManual = ChkManualBridge.IsChecked == true;
            settings.ForcedProfile = isManual ? "standard" : "none";
            settings.Save();

            if (isManual)
            {
                sessionManager.StopSession("Switching to manual bridge mode");
                string error;
                if (!sessionManager.StartSession(LocalizationManager.Get("Loc_ManualBridgeGameTitle"), "standard", settings, out error)) {
                    isInitialized = false;
                    ChkManualBridge.IsChecked = false;
                    isInitialized = true;
                    settings.ForcedProfile = "none";
                    settings.Save();
                }
                monitorService.ForceCheck();
            }
            else
            {
                if (sessionManager.IsSessionActive)
                {
                    sessionManager.StopSession("Exited manual bridge mode");
                }
                monitorService.ForceCheck();
            }
        }

        private async void OnUpdateDatabaseClick(object sender, RoutedEventArgs e)
        {
            TxtDatabaseInfo.Text = LocalizationManager.Get("Loc_Syncing");
            var success = await gameListService.FetchLatestFromCloudAsync();
            if (success)
            {
                int count = gameListService.TotalGamesLoaded;
                string gameWord = count > 1 ? LocalizationManager.Get("Loc_SyncSuccessGamesPlural") : LocalizationManager.Get("Loc_SyncSuccessGamesSingular");
                MessageBox.Show(this, LocalizationManager.Format("Loc_SyncSuccess", count, gameWord),
                                LocalizationManager.Get("Loc_AppName"), MessageBoxButton.OK, MessageBoxImage.Information);
            }
            else
            {
                UpdateDatabaseCount();
                MessageBox.Show(this, LocalizationManager.Get("Loc_SyncFailed"),
                                LocalizationManager.Get("Loc_AppName"), MessageBoxButton.OK, MessageBoxImage.Warning);
            }
        }

        private async void OnCheckUpdatesClick(object sender, RoutedEventArgs e)
        {
            if (updateChecker == null) return;
            UpdateInfo info = await updateChecker.CheckForUpdatesAsync(false);
            ShowUpdateBanner(info);
        }

        private void ShowUpdateBanner(UpdateInfo info)
        {
            latestUpdateInfo = info;
            if (info != null && info.HasUpdate)
            {
                TxtUpdateTitle.Text = LocalizationManager.Format("Loc_UpdateBannerTitle") + string.Format(" (v{0})", info.LatestVersion);
                TxtUpdateSubtitle.Text = LocalizationManager.Get("Loc_UpdateBannerSubtitle");
                BannerUpdate.Visibility = Visibility.Visible;
            }
            else
            {
                BannerUpdate.Visibility = Visibility.Collapsed;
            }
        }

        private void OnDownloadUpdateClick(object sender, RoutedEventArgs e)
        {
            if (updateChecker != null && latestUpdateInfo != null)
            {
                updateChecker.DownloadOrOpenRelease(latestUpdateInfo);
            }
        }

        private void OnOpenGameListClick(object sender, RoutedEventArgs e)
        {
            var win = new GameListWindow(gameListService, settings, learningService);
            win.Owner = this;
            win.ShowDialog();
            UpdateDatabaseCount();
        }

        private void OnOpenLearnedExecutablesClick(object sender, RoutedEventArgs e)
        {
            var win = new GameListWindow(gameListService, settings, learningService, initialTab: "learned");
            win.Owner = this;
            win.ShowDialog();
            UpdateDatabaseCount();
        }

        private void OnHideWindowClick(object sender, RoutedEventArgs e)
        {
            Hide();
        }
    }
}
