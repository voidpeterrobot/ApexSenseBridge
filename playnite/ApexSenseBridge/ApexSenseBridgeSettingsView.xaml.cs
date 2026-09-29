using System.Windows.Controls;

namespace ApexSenseBridge
{
    public partial class ApexSenseBridgeSettingsView : UserControl
    {
        public ApexSenseBridgeSettingsView()
        {
            InitializeComponent();
            GripControlsHost.Content = Shared.Apex6Beta.CreateControls(
                gain => { var send = ApexSenseBridge.LiveGripControl; if (send != null) send(gain); },
                () => { var status = ApexSenseBridge.GripStatus; return status == null ? "No Apex6 session" : status(); });
        }
    }
}
