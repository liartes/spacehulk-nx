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
    }

    public static class Loader
    {
        public static void Init()
        {
            var go = new GameObject("SpaceHulkSwitch");
            Object.DontDestroyOnLoad(go);
            go.AddComponent<SwitchMenus>();
            Native.Log("SwitchMenus ready");
        }
    }
}
