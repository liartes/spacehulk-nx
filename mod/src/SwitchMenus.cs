// SwitchMenus.cs -- the main menu's video options the Switch cannot honour,
// taken out of the panel.
//
// VideoPanel (the main menu's Options > Video, scene level4) offers the
// resolution list, a full-screen box and v-sync 0/1/2, as on a PC. On the
// Switch the port sets the picture (720p handheld, 1080p docked: config.ini)
// and its pacing, and a Screen.SetResolution from the game would fight it.
// So those three controls are hidden whenever the panel is open; the panel
// keeps them as they were (the current resolution, full screen), so its
// Apply button changes nothing there (ResolutionManager.UpdateResolution
// returns at once when nothing changed). Quality level, gamma, effects and
// dynamic lights stay: the port holds its texture and MSAA limits whatever
// the level (source/sh_quality.c). MIT.
using System;
using System.Reflection;
using UnityEngine;

namespace DcrMod
{
    public class SwitchMenus : MonoBehaviour
    {
        static readonly string[] Hidden = { "resolutionBar", "fullScreenCheckBox", "vsyncBar" };
        static Type videoPanel;
        float next;
        bool logged;

        void Start()
        {
            videoPanel = Type.GetType("FullControl.SpaceHulk.GUI.VideoPanel, Assembly-CSharp");
            if (videoPanel == null)
                Native.Log("SwitchMenus: no VideoPanel type in this game");
        }

        void Update()
        {
            if (videoPanel == null || Time.unscaledTime < next)
                return;
            next = Time.unscaledTime + 0.25f;
            foreach (UnityEngine.Object p in FindObjectsOfType(videoPanel))
                foreach (string f in Hidden)
                    Hide(p, f);
        }

        void Hide(UnityEngine.Object panel, string field)
        {
            FieldInfo fi = videoPanel.GetField(field, BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            Component c = fi != null ? fi.GetValue(panel) as Component : null;
            if (c == null || !c.gameObject.activeSelf)
                return;
            c.gameObject.SetActive(false);
            if (!logged)
                Native.Log("SwitchMenus: hid " + Path(c.transform) + " (siblings: " + Siblings(c.transform) + ")");
            if (field == Hidden[Hidden.Length - 1])
                logged = true;
        }

        static string Path(Transform t)
        {
            string p = t.name;
            for (Transform u = t.parent; u != null; u = u.parent)
                p = u.name + "/" + p;
            return p;
        }

        static string Siblings(Transform t)
        {
            if (t.parent == null)
                return "";
            string s = "";
            foreach (Transform c in t.parent)
                s += (s.Length > 0 ? ", " : "") + c.name;
            return s;
        }
    }
}
