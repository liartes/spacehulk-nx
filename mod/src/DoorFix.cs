// DoorFix.cs -- the doors' leaves drawn where the animation puts them.
//
// A door is DoorComp > Door01_PS3 (its Animation) > Door01A_LOD1,
// Door01B_LOD1 (the two leaves) and Door_01_Frame_LOD1. The DoorOpen clip
// slides the leaves 0.7 m apart, but the missions' scenes were built with
// the leaves statically batched: their MeshFilters hold a range of
// "Combined Mesh (root: scene) N", whose vertices are in world space and are
// drawn where they were baked, whatever the leaf's transform. So a door
// opened (the line of sight with it) and stayed drawn shut, even one open
// from the mission's start (hardware, 2026-10-08).
//
// The combined mesh lives on the GPU only (not readable: the leaves cannot
// be copied out of it), so each leaf gets a static batch root that rides on
// it: Unity draws a statically batched renderer with its
// staticBatchRootTransform's matrix (an internal property, reached by
// reflection). The root is a child of the leaf placed, in the baked pose
// (the door shut), at the world's origin: its matrix is identity while the
// door is shut and carries the leaf's movement when it opens. MIT.
using System;
using System.Collections.Generic;
using System.Reflection;
using UnityEngine;

namespace DcrMod
{
    public class DoorFix : MonoBehaviour
    {
        const string RootSuffix = "_batchroot";
        static Type doorType;
        static FieldInfo doorAnimation;
        static MethodInfo setRoot;
        readonly HashSet<int> done = new HashSet<int>();
        float next;
        int level = -1, leaves, failed;

        void Start()
        {
            doorType = Type.GetType("DoorComp, Assembly-CSharp");
            doorAnimation = doorType != null ? doorType.GetField("doorAnimation", BindingFlags.Instance | BindingFlags.Public) : null;
            PropertyInfo p = typeof(Renderer).GetProperty("staticBatchRootTransform", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
            setRoot = p != null ? p.GetSetMethod(true) : null; // internal: not public
            if (doorAnimation == null || setRoot == null)
                Native.Log("DoorFix: off (DoorComp.doorAnimation " + (doorAnimation != null) + ", Renderer.staticBatchRootTransform " + (setRoot != null) + ")");
        }

        void Update()
        {
            if (doorAnimation == null || setRoot == null)
                return;
            if (Application.loadedLevel != level)
            {
                if (leaves + failed > 0)
                    Native.Log("DoorFix: level " + level + ": " + leaves + " door leaves unbatched" + (failed > 0 ? ", " + failed + " failed" : ""));
                level = Application.loadedLevel;
                done.Clear();
                leaves = failed = 0;
                next = 0;
            }
            // doors come with the level (and the mission editor's): looked
            // for every second, each fixed once
            if (Time.unscaledTime < next)
                return;
            next = Time.unscaledTime + 1f;
            foreach (UnityEngine.Object o in FindObjectsOfType(doorType))
                if (done.Add(o.GetInstanceID()))
                    Fix((Component)o);
        }

        void Fix(Component door)
        {
            var anim = doorAnimation.GetValue(door) as Animation;
            if (anim == null || anim["DoorOpen"] == null)
                return;
            // the baked pose (DoorOpen at 0: shut), then the door as it was
            Transform[] all = anim.GetComponentsInChildren<Transform>(true);
            var pos = new Vector3[all.Length];
            var rot = new Quaternion[all.Length];
            for (int i = 0; i < all.Length; i++) { pos[i] = all[i].localPosition; rot[i] = all[i].localRotation; }
            anim.Sample("DoorOpen", 0f);
            foreach (MeshRenderer r in anim.GetComponentsInChildren<MeshRenderer>(true))
            {
                var mf = r.GetComponent<MeshFilter>();
                Mesh m = mf ? mf.sharedMesh : null;
                if (m == null || !m.name.StartsWith("Combined Mesh") || r.name.IndexOf("Frame") >= 0 ||
                    r.transform.Find(r.name + RootSuffix) != null)
                    continue;
                try
                {
                    var t = new GameObject(r.name + RootSuffix).transform;
                    t.parent = r.transform;
                    t.position = Vector3.zero;
                    t.rotation = Quaternion.identity;
                    Vector3 ls = r.transform.lossyScale;
                    t.localScale = new Vector3(1f / ls.x, 1f / ls.y, 1f / ls.z);
                    setRoot.Invoke(r, new object[] { t });
                    leaves++;
                }
                catch (Exception e)
                {
                    if (failed++ == 0)
                        Native.Log("DoorFix: " + door.name + "/" + r.name + ": " + e.GetType().Name + ": " + e.Message);
                }
            }
            for (int i = 0; i < all.Length; i++) { all[i].localPosition = pos[i]; all[i].localRotation = rot[i]; }
        }
    }
}
