// Loader.cs -- the port's own C# (built into mod/dcrmod.dll by mod/build_mod.sh,
// carried in the program and loaded into the game's Mono by source/dcr_mod.c,
// which calls DcrMod.Loader.Init() once Assembly-CSharp is loaded). MIT.
using System.Runtime.CompilerServices;
using UnityEngine;

namespace DcrMod
{
    public static class Native
    {
        // "[mod] <text>" in debug.log (dcr_mod.c)
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void Log(string text);

        // target's compiled code jumps to repl's (source/dcr_mod.c); 0 done
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern int Detour(System.IntPtr target, System.IntPtr repl, System.IntPtr orig);

        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern long TicksMs();

        // the game folder on the SD card (sdmc:/switch/spacehulk_nx)
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern string GameRoot();
    }

    public static class Loader
    {
        static GameObject go;

        public static void Init()
        {
            UndoSave.Install();
            Make();
        }

        static void Make()
        {
            go = new GameObject("SpaceHulkSwitch");
            Object.DontDestroyOnLoad(go);
            go.AddComponent<SwitchMenus>();
            go.AddComponent<DoorFix>();
        }

        // every 30 frames (source/dcr_mod.c): the game's first scene
        // (sceneLoaderAndroid) destroys the object Init made, DontDestroyOnLoad
        // or not, before any of its components started (hardware, 2026-10-08):
        // made again whenever it is gone
        public static void Tick()
        {
            if (go != null)
                return;
            Make();
            Native.Log("SwitchMenus and DoorFix ready (level " + Application.loadedLevelName + ")");
        }
    }
}
