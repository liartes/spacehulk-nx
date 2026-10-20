// UndoTrace.cs -- where the undo button's time goes.
//
// The snapshot before each move is off the main thread (UndoSave.cs), yet the
// player still waits after pressing undo (2026-10-20), and no frame of the
// undo crossed the port's 120 ms log line. GameController.UndoAction is
// redirected here: the same steps as the game's (UndoAction + ApplyState),
// each one timed, then the frames are followed until the game hands the
// marines back (StateManager.State == UserInput) -- how long, how many frames,
// the longest one. The first 20 undos of a session are logged. With [debug]
// undo_trace only. It found the frame per unit of GameManager.InstantiateUnits
// (FastViewUpdate.cs): 2.4 s, then 0.2 s. MIT.
using System;
using System.Reflection;
using FullControl.SpaceHulk.Serialization;
using PS3;
using TXEngine.NetworkedCombatSystem;
using TXEngine.NetworkedCombatSystem.Model;
using UnityEngine;

namespace DcrMod
{
    public class UndoTrace : MonoBehaviour
    {
        static FieldInfo undoState;
        static int count;

        // the undo being followed: from its start until UserInput
        static bool following;
        static long tStart, tLast;
        static int frames, longest;
        static string steps;

        public static void Install()
        {
            if (Native.Config("debug.undo_trace", 0) == 0)
                return;
            const BindingFlags F = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
            undoState = typeof(GameController).GetField("undoState", F);
            MethodInfo target = typeof(GameController).GetMethod("UndoAction", F);
            MethodInfo repl = typeof(UndoTrace).GetMethod("UndoAction", BindingFlags.Static | BindingFlags.NonPublic);
            if (undoState == null || target == null || repl == null)
            {
                Native.Log("UndoTrace: GameController.UndoAction not found");
                return;
            }
            int r = Native.Detour(target.MethodHandle.Value, repl.MethodHandle.Value, IntPtr.Zero);
            Native.Log("UndoTrace: " + (r == 0 ? "undo timed" : "not hooked (" + r + ")"));
        }

        // in place of GameController.UndoAction (the instance first)
        static void UndoAction(GameController self)
        {
            long t0 = Native.TicksMs();
            byte[] state = (byte[])undoState.GetValue(self);
            undoState.SetValue(self, null);
            GameManager.Instance.InterruptAsyncEvents();
            long t1 = Native.TicksMs();
            // GameController.ApplyState(state, makeInnocent: true)
            StateManager.Apply(GameTransitions.UserAction);
            StateManager.SetState(GameStates.Processing);
            SHModel model = SHSerializer.LoadModel(state);
            long t2 = Native.TicksMs();
            model.ForceInnocent();
            SHModel current = SHModel.Instance;
            Combat.Model = model;
            model.SoftLoadAfterDeserialization(current);
            long t3 = Native.TicksMs();
            Manager.MissionControlManager.Deserialize(model.MissionControlData);
            long t4 = Native.TicksMs();
            model.SendUpdate();
            long t5 = Native.TicksMs();
            Singleton<AISharedData>.Instance.FlagAsOutdatedLoSAll();
            Singleton<AISharedData>.Instance.UpdateData();
            long t6 = Native.TicksMs();
            if (count++ >= 20)
                return;
            steps = "interrupt " + (t1 - t0) + ", load " + (t2 - t1) + " (" + (state.Length / 1024) + " KB), swap " + (t3 - t2) +
                    ", mission control " + (t4 - t3) + ", update " + (t5 - t4) + ", AI line of sight " + (t6 - t5) + " ms";
            following = true;
            tStart = t0;
            tLast = t6;
            frames = 0;
            longest = (int)(t6 - t0);
        }

        void Update()
        {
            if (!following)
                return;
            long now = Native.TicksMs();
            frames++;
            if (now - tLast > longest)
                longest = (int)(now - tLast);
            tLast = now;
            GameStates s = StateManager.State;
            if (s != GameStates.UserInput && now - tStart < 15000)
                return;
            following = false;
            Native.Log("UndoTrace: " + (s == GameStates.UserInput ? "back to the player" : "still " + s) + " after " + (now - tStart) +
                       " ms, " + frames + " frames (longest " + longest + " ms); " + steps);
        }
    }
}
