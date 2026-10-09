// PerfBench.cs -- what each graphics setting costs, measured in play.
//
// [debug] perf_bench: in a mission, every 20 s the next variant of a cycle
// (the pixel lights: 2, 1, 0)
// is put in place, and at its end the variant's frame rate and draw calls per
// frame (the port's counter, source/sh_glcount.c) are logged. Cycling, the
// game's own ups and downs (camera, the Genestealers' turn) fall on every
// variant alike. Each cycle starts with a census of what the scene animates,
// lights and draws. Everything is put back between variants. MIT.
using System;
using System.Collections.Generic;
using UnityEngine;

namespace DcrMod
{
    public class PerfBench : MonoBehaviour
    {
        // pixel lights 2 (the port's default), 1, 0; the first bench (2026-10-09:
        // shadows, animation culling) found the pixel lights to be what costs
        static readonly string[] Names = { "lights2", "lights1", "lights0" };
        const float Period = 20f;

        bool on, inMission;
        float next, start, check;
        int variant = -1, frames0;
        long draws0;
        int baseLights;

        void Start()
        {
            on = Native.Config("debug.perf_bench", 0) != 0;
            if (on)
                Native.Log("PerfBench: on (variants every " + Period + " s in missions)");
        }

        void Update()
        {
            if (!on)
                return;
            float now = Time.realtimeSinceStartup;
            if (now >= check)
            {
                check = now + 1f;
                bool m = FindObjectOfType(typeof(GameController)) != null && Type.GetType("DoorComp, Assembly-CSharp") != null &&
                         FindObjectOfType(Type.GetType("DoorComp, Assembly-CSharp")) != null;
                if (m != inMission)
                {
                    inMission = m;
                    if (m)
                    {
                        int pl = Native.Config("graphics.pixel_lights", -1);
                        baseLights = pl >= 0 ? pl : 4; // -1: the top level's
                        variant = -1;
                        next = now + 5f; // the mission settles first
                        Native.Log("PerfBench: mission (pixel lights " + baseLights + ")");
                    }
                    else
                    {
                        Restore();
                        variant = -1;
                    }
                }
            }
            if (!inMission || now < next)
                return;
            if (variant >= 0)
            {
                float secs = now - start;
                int frames = Time.frameCount - frames0;
                long draws = Native.DrawCount() - draws0;
                Native.Log("PerfBench: " + Names[variant] + " " + secs.ToString("F1") + " s: " + (frames / secs).ToString("F1") +
                           " fps, " + (frames > 0 ? draws / frames : 0) + " draws/frame");
            }
            Restore();
            variant = (variant + 1) % Names.Length;
            if (variant == 0)
                Census();
            Apply(variant);
            start = now;
            next = now + Period;
            frames0 = Time.frameCount;
            draws0 = Native.DrawCount();
        }

        void Apply(int v)
        {
            Native.SetPixelLights(2 - v);
        }

        void Restore()
        {
            Native.SetPixelLights(baseLights);
        }

        static void Census()
        {
            int anims = 0, always = 0, playing = 0, states = 0;
            foreach (Animation a in FindObjectsOfType(typeof(Animation)))
            {
                anims++;
                if (a.cullingType == AnimationCullingType.AlwaysAnimate)
                    always++;
                if (a.isPlaying)
                    playing++;
                foreach (AnimationState s in a)
                    states++;
            }
            int animators = FindObjectsOfType(typeof(Animator)).Length;
            int skinned = 0, bones = 0;
            foreach (SkinnedMeshRenderer r in FindObjectsOfType(typeof(SkinnedMeshRenderer)))
            {
                skinned++;
                bones += r.bones.Length;
            }
            int renderers = FindObjectsOfType(typeof(Renderer)).Length;
            int lights = 0;
            foreach (Light l in FindObjectsOfType(typeof(Light)))
                if (l.enabled)
                    lights++;
            int particles = FindObjectsOfType(typeof(ParticleSystem)).Length;
            Native.Log("PerfBench: census: " + anims + " Animation (" + always + " always animate, " + playing + " playing, " + states +
                       " states), " + animators + " Animator, " + skinned + " skinned meshes (" + bones + " bones), " + renderers +
                       " renderers, " + lights + " lights, " +
                       particles + " particle systems");
        }
    }
}
