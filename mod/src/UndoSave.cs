// UndoSave.cs -- the undo snapshot taken off the main thread.
//
// Before each undoable action of the player's marines the game copies its
// whole model for the undo button: GameController.SaveUndoState starts the
// coroutine SaveUndoThread, which runs SHSerializer.SaveModel -- a
// BinaryFormatter pass over the model -- on the main thread, then waits for
// it (while savingUndo). On the Switch that froze the picture for 300-500 ms
// at each move (hardware, 2026-10-09: 32 of the session's 76 frames over
// 300 ms came right after the serializer's "Not being compressed!!!"). The
// multiplayer path already does the same SaveModel on a thread of its own
// (SaveSendThread).
//
// The coroutine's MoveNext is redirected here (SaveUndoThread itself is too
// small: Mono inlines it into its caller, and a hook on it never ran --
// hardware run 39). The model is prepared on the main
// thread (PrepareSerialization: the mission control's data, which may touch
// Unity), serialized on a thread, and undoState / savingUndo set when it is
// done; the game's loop waits for savingUndo as before, the frames going on
// meanwhile. Should the thread fail, the snapshot is taken on the main
// thread as the game did. MIT.
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Threading;
using FullControl.SpaceHulk.Serialization;
using TXEngine.NetworkedCombatSystem.Model;
using UnityEngine;

namespace DcrMod
{
    public static class UndoSave
    {
        static FieldInfo undoState, savingUndo, owner;
        static readonly Dictionary<object, Job> jobs = new Dictionary<object, Job>();
        static int count;

        public static void Install()
        {
            const BindingFlags F = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
            Type gc = typeof(GameController);
            undoState = gc.GetField("undoState", F);
            savingUndo = gc.GetField("savingUndo", F);
            // the coroutine object SaveUndoThread returns (mcs: <SaveUndoThread>c__IteratorN)
            Type iter = null;
            foreach (Type t in gc.Assembly.GetTypes())
                if (t.DeclaringType == gc && t.Name.StartsWith("<SaveUndoThread>"))
                    iter = t;
            MethodInfo target = iter != null ? iter.GetMethod("MoveNext", F) : null;
            if (iter != null)
                foreach (FieldInfo f in iter.GetFields(F))
                    if (f.FieldType == gc)
                        owner = f;
            MethodInfo repl = typeof(UndoSave).GetMethod("MoveNext", BindingFlags.Static | BindingFlags.NonPublic);
            if (target == null || owner == null || undoState == null || savingUndo == null || repl == null)
            {
                Native.Log("UndoSave: GameController's SaveUndoThread coroutine not found: undo snapshots stay on the main thread");
                return;
            }
            int r = Native.Detour(target.MethodHandle.Value, repl.MethodHandle.Value, IntPtr.Zero);
            Native.Log("UndoSave: " + (r == 0 ? "undo snapshots taken on a thread (" + iter.Name + ")" : "not hooked (" + r + ")"));
        }

        // in place of <SaveUndoThread>c__IteratorN.MoveNext (an instance
        // method: the coroutine object comes first); true while the snapshot
        // is being taken
        static bool MoveNext(object iter)
        {
            Job j;
            if (!jobs.TryGetValue(iter, out j))
            {
                j = new Job((GameController)owner.GetValue(iter));
                jobs[iter] = j;
            }
            if (j.Running())
                return true;
            jobs.Remove(iter);
            return false;
        }

        // One thread for all the snapshots, made once and never ending: a
        // thread per snapshot crashed the game (run 40) -- the runtime frees a
        // finished thread's stack when the next thread is made, and Mono's GC
        // still scanned it.
        static class Worker
        {
            static readonly object gate = new object();
            static Action next;
            static Thread thread;

            public static void Run(Action job, Action<string> fail)
            {
                try
                {
                    lock (gate)
                    {
                        if (thread == null)
                        {
                            thread = new Thread(Loop);
                            thread.IsBackground = true;
                            thread.Start();
                        }
                        next = job;
                        Monitor.Pulse(gate);
                    }
                }
                catch (Exception e)
                {
                    fail("worker: " + e.Message);
                }
            }

            static void Loop()
            {
                for (;;)
                {
                    Action job;
                    lock (gate)
                    {
                        while (next == null)
                            Monitor.Wait(gate, 1000);
                        job = next;
                        next = null;
                    }
                    job();
                }
            }
        }

        class Job
        {
            readonly GameController self;
            readonly long t0 = Native.TicksMs();
            long tPrep, tSer;
            volatile int state; // 0 running, 1 done, 2 failed
            string error;

            public Job(GameController self)
            {
                this.self = self;
                SHModel model = SHModel.Instance;
                try
                {
                    model.PrepareSerialization(true);
                }
                catch (Exception e)
                {
                    Fail("PrepareSerialization: " + e.Message);
                    return;
                }
                tPrep = Native.TicksMs();
                Worker.Run(() =>
                {
                    try
                    {
                        byte[] data = SHSerializer.ObjectToCompressedByteArray(model);
                        if (data == null)
                            throw new Exception("the serializer returned nothing");
                        tSer = Native.TicksMs();
                        undoState.SetValue(self, data);
                        state = 1;
                    }
                    catch (Exception e)
                    {
                        error = e.GetType().Name + ": " + e.Message;
                        state = 2;
                    }
                }, Fail);
            }

            void Fail(string why)
            {
                error = why;
                state = 2;
            }

            // on the main thread, once a frame: done -> savingUndo cleared;
            // failed -> the game's own way, here
            public bool Running()
            {
                if (state == 0)
                    return true;
                if (state == 2)
                {
                    Native.Log("UndoSave: " + error + " -- this snapshot on the main thread");
                    undoState.SetValue(self, SHSerializer.SaveModel(true));
                }
                else if (count++ < 5)
                    Native.Log("UndoSave: snapshot in " + (tSer - t0) + " ms (prepared on the main thread in " + (tPrep - t0) + " ms)");
                savingUndo.SetValue(self, false);
                return false;
            }
        }
    }
}
