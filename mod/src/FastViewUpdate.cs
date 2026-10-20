// FastViewUpdate.cs -- the view rebuilt without a frame per unit.
//
// After the undo button (and every other full view update) the game rebuilds
// the scene in GameManager.InstantiateUnits, a coroutine that waits a frame
// after each unit of the mission -- marines, Genestealers, objects -- even
// when the unit is already on the board and nothing is made. With ~70 units
// that is ~70 frames: 2.4 s at the port's 30 fps (UndoTrace, hardware
// 2026-10-20: every undo 2413-2433 ms, 71 frames, of which the model's load
// was 102 ms). The coroutine's MoveNext is redirected here (as UndoSave's:
// a hook on InstantiateUnits itself could be inlined away): the same units
// made in the same order, but a frame waited only after a unit was really
// made, so a mission's loading still moves its progress bar unit by unit.
// MIT.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using TXEngine.NetworkedCombatSystem.Model;

namespace DcrMod
{
    public static class FastViewUpdate
    {
        static FieldInfo owner, unitObjects, complete;
        static MethodInfo newUnit;
        static readonly Dictionary<object, Job> jobs = new Dictionary<object, Job>();

        public static void Install()
        {
            const BindingFlags F = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
            Type gm = typeof(GameManager);
            unitObjects = gm.GetField("unitObjects", F);
            complete = gm.GetField("InstantiateUnitsComplete", F);
            newUnit = gm.GetMethod("NewUnit", F, null, new[] { typeof(SHUnit), typeof(bool) }, null);
            // the coroutine object InstantiateUnits returns (mcs: <InstantiateUnits>c__IteratorN)
            Type iter = null;
            foreach (Type t in gm.Assembly.GetTypes())
                if (t.DeclaringType == gm && t.Name.StartsWith("<InstantiateUnits>"))
                    iter = t;
            MethodInfo target = iter != null ? iter.GetMethod("MoveNext", F) : null;
            if (iter != null)
                foreach (FieldInfo f in iter.GetFields(F))
                    if (f.FieldType == gm)
                        owner = f;
            MethodInfo repl = typeof(FastViewUpdate).GetMethod("MoveNext", BindingFlags.Static | BindingFlags.NonPublic);
            if (target == null || owner == null || unitObjects == null || complete == null || newUnit == null || repl == null)
            {
                Native.Log("FastViewUpdate: GameManager's InstantiateUnits coroutine not found: a frame per unit as before");
                return;
            }
            int r = Native.Detour(target.MethodHandle.Value, repl.MethodHandle.Value, IntPtr.Zero);
            Native.Log("FastViewUpdate: " + (r == 0 ? "a frame only per unit made (" + iter.Name + ")" : "not hooked (" + r + ")"));
        }

        // in place of <InstantiateUnits>c__IteratorN.MoveNext (the coroutine
        // object first); true: wait a frame
        static bool MoveNext(object iter)
        {
            Job j;
            if (!jobs.TryGetValue(iter, out j))
            {
                j = new Job((GameManager)owner.GetValue(iter));
                jobs[iter] = j;
            }
            if (j.Step())
                return true;
            jobs.Remove(iter);
            return false;
        }

        class Job
        {
            readonly GameManager self;
            readonly List<SHUnit> units = new List<SHUnit>();
            readonly float increment;
            int next;

            public Job(GameManager self)
            {
                this.self = self;
                SHModel model = SHModel.Instance;
                foreach (SHUnit u in model.Units)
                    units.Add(u);
                foreach (SHUnit u in model.ObjectUnits)
                    units.Add(u);
                increment = 1f / units.Count;
            }

            // the next units up to the first one made; false when all are done
            public bool Step()
            {
                Hashtable objects = (Hashtable)unitObjects.GetValue(self);
                while (next < units.Count)
                {
                    SHUnit u = units[next++];
                    bool had = objects[u.Id] != null;
                    newUnit.Invoke(self, new object[] { u, false });
                    self.LoadProgress += increment;
                    if (!had && objects[u.Id] != null)
                        return true;
                }
                complete.SetValue(self, true);
                self.LoadProgress = 1f;
                return false;
            }
        }
    }
}
