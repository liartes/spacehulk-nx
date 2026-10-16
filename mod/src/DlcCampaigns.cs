// DlcCampaigns.cs -- [game] unlock_dlc_campaigns: the campaigns this Android
// build carries but keeps locked.
//
// Data/Levels/Campaigns.xml lists, besides the main campaign, Messenger of
// Purgatory (the mobile in-app purchase) and the PC DLC campaigns Sword of
// Halcyon, Defilement of Honour and Fangs of Fenris; their missions' data and
// their "mobile" scenes are all in the build (2026-10-16). They are only
// locked: defaultUnlockedMobile false, and nothing on Android calls the
// game's own unlock (DLCSystem -> MissionController.UnlockCampaign).
// Harbinger of Torment has no scenes in this build (enabledMobile false) and
// stays out, as the game's campaign screen already leaves it.
//
// For players who own those campaigns (Steam DLC, the mobile purchase): off
// by default. Unlocked again whenever the main menu is up, so that a save
// loaded since cannot lock them back. MIT.
using UnityEngine;

namespace DcrMod
{
    public static class DlcCampaigns
    {
        static readonly string[] Keys = { "MessengerOfPurgatory", "SwordOfHalcyon", "DefilementOfHonour", "FangsOfFenris" };
        static int on = -1;

        // Loader.Tick, every 30 frames
        public static void Tick()
        {
            if (on < 0)
                on = Native.Config("game.unlock_dlc_campaigns", 0) != 0 ? 1 : 0;
            if (on == 0 || Application.loadedLevelName != "MainMenu")
                return;
            MissionController mc = MissionController.Instance;
            foreach (string k in Keys)
                if (mc.GetCampaign(k) != null && !mc.IsCampaignUnlocked(k))
                {
                    mc.UnlockCampaign(k, false);
                    Native.Log("DlcCampaigns: " + k + " unlocked");
                }
        }
    }
}
