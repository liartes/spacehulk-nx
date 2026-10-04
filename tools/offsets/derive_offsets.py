#!/usr/bin/env python3
"""
derive_offsets.py -- generate source/dcr_offsets.h from the game's own binaries.

Disney Crossy Road: SEA (Unity 5.6.4f1; the same script still re-derives the
Unity 2017.4.17f1 world-wide release byte for byte), armeabi-v7a, Mono backend.
Unity 5.6's libunity reaches its data GOT-relative, 2017.4's pc-relative: the
arm32.py trackers resolve both.

Every address emitted is found STRUCTURALLY from the shipped libunity.so /
libmono.so -- by unique strings, PLT call targets, and instruction shape -- so a
game update that moves code still re-derives correctly. Every address carries a
4-word guard (its leading instruction words); consumers verify the guard before
they hook or patch and SKIP on mismatch, so a stale offset is a logged line, not
a wild write. This is the 32-bit / Mono counterpart of the Drive Ahead port's
tools/offsets/derive_offsets.py.

Usage:
    python3 derive_offsets.py <path-to-lib/armeabi-v7a> [-o source/dcr_offsets.h]

The three categories the port needs (and the user called out):
    * 15 time offsets  = 11 Time icall bindings + 4 TimeManager entry points
    * 4  GC offsets    = Mono bdwgc thread-suspend signals + ack sem + wrapper
    * vsync / "swappy" = Choreographer VSYNC enable + the counter/mutex/cond
                         WaitVSync itself waits on
"""
import sys
import os
import struct
import argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from arm32 import Arm32, hexguard

# The 11 Time icalls this port hooks (same set as Drive Ahead). The engine
# declares 23; hooking these covers every managed Time.* the game reads that
# matters across sleep/HOME. Value = the TimeManager field the stock getter
# loads, used to VALIDATE that we matched the right function.
TIME_HOOKS = {
    "get_time": ("f64", 0x80),
    "get_deltaTime": ("f32", 0x98),
    "get_unscaledTime": ("f64", 0x90),
    "get_unscaledDeltaTime": ("f32", 0x9C),
    "get_fixedDeltaTime": ("f32", 0x38),
    "set_fixedDeltaTime": (None, None),      # tail-calls SetFixedDeltaTime
    "get_maximumParticleDeltaTime": ("f32", 0xEC),
    "get_timeScale": ("f32", 0xE4),
    "set_timeScale": (None, None),           # tail-calls SetTimeScale
    "get_frameCount": ("i32", 0xB4),
    "get_realtimeSinceStartup": (None, None),  # calls GetRealtimeSinceStartup
}


def bl_target(w, pc):
    if (w & 0x0E000000) != 0x0A000000 or (w >> 28) == 0xF:
        return None
    imm = w & 0xFFFFFF
    if imm & 0x800000:
        imm -= 0x1000000
    return (pc + 8 + imm * 4) & 0xFFFFFFFF


def is_bl(w):
    return (w & 0x0F000000) == 0x0B000000


def is_b(w):
    return (w & 0x0F000000) == 0x0A000000 and (w >> 28) == 0xE


def load_field(u, fn):
    """Decode the field a Time getter loads at its 3rd instruction:
    vldr d/s [r0,#imm*4] or ldr/ldrb [r0,#imm]. Returns (kind, offset) or (None,None)."""
    w = u.u32(fn + 8)
    if (w & 0x0FBF0F00) == 0x0D900B00:      # vldr d,[r0,#imm*4]
        return "f64", (w & 0xFF) * 4
    if (w & 0x0FBF0F00) == 0x0D900A00:      # vldr s,[r0,#imm*4]
        return "f32", (w & 0xFF) * 4
    if (w & 0x0FF00000) == 0x05900000:      # ldr r,[r0,#imm]
        return "i32", w & 0xFFF
    if (w & 0x0FF00000) == 0x05D00000:      # ldrb r,[r0,#imm]
        return "u8", w & 0xFFF
    return None, None


# Unity 5.5+ only (5.3 has no Time.maximumParticleDeltaTime).
OPTIONAL_TIME_HOOKS = {"get_maximumParticleDeltaTime"}


def _time_icalls_table(u, slots, in_text):
    """Unity 5.6 / 2017.4: a flat 4-byte name-pointer array, and the parallel
    function-pointer array right after it."""
    name_base = min(slots)
    names = []
    i = 0
    while True:
        try:
            s = u.cstr(u.u32(name_base + 4 * i))
        except Exception:
            break
        if not s.startswith("UnityEngine.Time::"):
            break
        names.append(s.split("::", 1)[1])
        i += 1
    N = len(names)
    # The parallel 4-byte function-pointer array: the first run of N in-.text
    # pointers at/after the end of the name array.
    fb = name_base + 4 * N
    while fb < name_base + 4 * N + 0x40:
        if all(in_text(u.u32(fb + 4 * j)) for j in range(N)):
            break
        fb += 4
    return names, {names[j]: u.u32(fb + 4 * j) for j in range(N)}


def _time_icalls_registered(u, anchor):
    """Unity 5.3: no table; a registration function calls
    mono_add_internal_call(name, fn) once per icall, both arguments
    GOT-relative. The Time class's run of calls is read off its registers."""
    refs = u.data_refs(anchor)
    if not refs:
        raise SystemExit("UnityEngine.Time::get_time: no reference -- game updated?")
    fn = u.func_start(refs[0])
    names, funcs = [], {}
    for e in u.track_globals(fn, 1024):
        if e[0] != "call" or 0 not in e[3] or 1 not in e[3]:
            continue
        try:
            s = u.cstr(e[3][0])
        except Exception:
            continue
        if not s.startswith("UnityEngine.Time::"):
            if names:
                break
            continue
        nm = s.split("::", 1)[1]
        names.append(nm)
        funcs[nm] = e[3][1]
    return names, funcs


# ---------------------------------------------------------------------------
def derive_time(u):
    """Time icall table + the 4 TimeManager entry points, from the icall name
    array anchored on the unique "UnityEngine.Time::get_time" string."""
    base, d = u.text()
    in_text = lambda a: base <= a < base + len(d)

    anchor = u.find_str("UnityEngine.Time::get_time")[0]
    slots = u.words_equal(anchor)
    if slots:
        names, funcs = _time_icalls_table(u, slots, in_text)
        flat = True
    else:
        names, funcs = _time_icalls_registered(u, anchor)
        flat = False
    N = len(names)

    # GetTimeManager: the bl at the 2nd instruction of get_time's body.
    gtm = bl_target(u.u32(funcs["get_time"] + 4), funcs["get_time"] + 4)

    # Validate every hooked getter: right field, or (for set_*/realtime) right shape.
    hooks, fields = {}, {}
    for nm, (kind, fld) in TIME_HOOKS.items():
        if nm not in funcs:
            if nm in OPTIONAL_TIME_HOOKS:
                continue                         # newer Unity only
            raise SystemExit("Time icall %r not found -- game updated?" % nm)
        fn = funcs[nm]
        if fld is not None and not flat:
            # Unity 5.3: another TimeManager layout -- the field is read off
            # the getter (push / bl GetTimeManager / load), not checked.
            k2, f2 = load_field(u, fn)
            if f2 is None or not is_bl(u.u32(fn + 4)):
                raise SystemExit("Time icall %r: not a field getter -- game updated?" % nm)
            fields[nm] = f2
            note = "field +0x%x (read off the getter)" % f2
        elif fld is not None:
            # The icall returns via r0 under the soft-float ABI, so a float
            # field is loaded with a plain `ldr` too -- validate the OFFSET, not
            # the load width.
            _, f2 = load_field(u, fn)
            ok = (f2 == fld)
            note = "field +0x%x %s" % (f2, "OK" if ok else "MISMATCH exp +0x%x" % fld)
        else:
            note = "control-flow getter"
        hooks[nm] = (fn, u.guard(fn), note)

    # TimeManager entry points.
    def nth_bl(fn, n):
        c = 0
        for k in range(0, 24):
            w = u.u32(fn + 4 * k)
            if is_bl(w):
                if c == n:
                    return bl_target(w, fn + 4 * k)
                c += 1
        return None

    def tail_b(fn):
        for k in range(0, 24):
            w = u.u32(fn + 4 * k)
            if is_b(w):
                return bl_target(w, fn + 4 * k)
        return None

    entries = {
        "GetTimeManager": gtm,
        "GetRealtimeSinceStartup": nth_bl(funcs["get_realtimeSinceStartup"], 1),
        "SetTimeScale": tail_b(funcs["set_timeScale"]),
        "SetFixedDeltaTime": tail_b(funcs["set_fixedDeltaTime"]),
    }
    entries = {k: (v, u.guard(v)) for k, v in entries.items()}

    # Field map for dcr_time.c pass-throughs, read off the getters.
    fld = lambda nm: fields.get(nm, TIME_HOOKS[nm][1])
    fieldmap = {"FIXEDDELTA": fld("get_fixedDeltaTime")}
    if "get_maximumParticleDeltaTime" in hooks:
        fieldmap["MAXPARTICLEDELTA"] = fld("get_maximumParticleDeltaTime")
    fieldmap["TIMESCALE"] = fld("get_timeScale")
    fieldmap["FRAMECOUNT"] = fld("get_frameCount")
    return dict(names=names, hooks=hooks, entries=entries, fieldmap=fieldmap, N=N)


# ---------------------------------------------------------------------------
def derive_vsync(u):
    """WaitVSync's mutex/cond/counter and the Choreographer VSYNC-enable fn.

    WaitVSync (found via a cond_wait site whose function also calls
    mutex_lock/unlock) is: lock(&m) / while(counter<target) cond_wait(&cond,&m)
    / unlock(&m). The three globals are read directly out of its pc-relative
    literals, so they cannot disagree with the code that waits on them.
    """
    base, d = u.text()
    cw = set(u.plt_sites("pthread_cond_wait"))
    lock = set(u.plt_sites("pthread_mutex_lock"))
    unlock = set(u.plt_sites("pthread_mutex_unlock"))

    # WaitVSync is the cond_wait user that (a) locks+unlocks a mutex, (b) saves
    # its target argument `mov r4, r0`, and (c) spins `while(counter < r4)` --
    # i.e. a `cmp _, r4` after the wait. That distinguishes the frame pacer from
    # the ~14 other cond_wait users (generic job/semaphore waits).
    wait = None
    for site, _ in u.bl_targets_to(cw):
        fn = u.func_start(site)
        if fn is None or (site - fn) >= 0x80:
            continue
        ws = [u.u32(fn + 4 * k) for k in range((site - fn) // 4 + 16)]
        # unlock is frequently a tail `b`, not `bl`, so accept either.
        has_lock = any((is_bl(w) or is_b(w)) and bl_target(w, fn + 4 * k) in lock
                       for k, w in enumerate(ws))
        has_unlock = any((is_bl(w) or is_b(w)) and bl_target(w, fn + 4 * k) in unlock
                         for k, w in enumerate(ws))
        saves_arg = any(w == 0xE1A04000 for w in ws[:4])          # mov r4, r0
        cmp_arg = any((w & 0x0FF00FF0) == 0x01500000 and (w & 0xF) == 4
                      for w in ws)                                  # cmp _, r4
        if has_lock and has_unlock and saves_arg and cmp_arg:
            wait = fn
            break
    if wait is None:
        return None

    # The three globals, from the registers at the calls: r0 at the first
    # mutex_lock is the mutex, r0 at cond_wait the condition, and the counter
    # is the word loaded from a known address and compared with the target.
    # Unity 2017.4 addresses them pc-relative (mutex, cond, counter in a row);
    # Unity 5.6 GOT-relative, as fields of one struct (counter +0x14, mutex
    # +0x38, cond +0x3c) -- u.track_globals resolves both the same way.
    mutex = cond = counter = None
    ev = u.track_globals(wait, 24)
    for k, e in enumerate(ev):
        if e[0] == "call" and e[2] in lock and mutex is None:
            mutex = e[3].get(0)
        elif e[0] == "call" and e[2] in cw and cond is None:
            cond = e[3].get(0)
        elif e[0] == "load" and counter is None:
            w = u.u32(e[1] + 4)                                 # cmp rt, r4 next
            if (w & 0x0FF00FF0) == 0x01500000 and (w & 0xF) == 4 and ((w >> 16) & 0xF) == e[2]:
                counter = e[3]
    if mutex is not None and cond is None:
        cond = mutex + 4
    if mutex is not None and counter is None:
        counter = mutex + 8

    # Choreographer VSYNC enable -- patch to `ret`. Anchored on its log string.
    s = u.find_str("Choreographer available: Enabling VSYNC timing")
    choreo = None
    if s:
        refs = u.data_refs(s[0])
        if refs:
            choreo = u.func_start(refs[0])

    out = dict(wait=wait, mutex=mutex, cond=cond, counter=counter,
               wait_guard=u.guard(wait))
    if choreo:
        out["choreo"] = choreo
        out["choreo_guard"] = u.guard(choreo)
        stop = derive_choreo_stop(u, choreo)
        if stop:
            out["choreo_stop"] = stop
            out["choreo_stop_guard"] = u.guard(stop)
    return out


def _pcrel_globals(u, fn, n=24, loads_only=False):
    """Absolute addresses a function reaches via `ldr Rt,[pc,#i]` + `add|ldr Rx,[pc,Rt]`.
    loads_only keeps the `ldr Rx,[pc,Rt]` form: data the function READS (a
    global variable), not addresses it merely takes (strings, assert paths)."""
    lit, out = {}, []
    for k in range(n):
        pc = fn + 4 * k
        w = u.u32(pc)
        if (w & 0x0F7F0000) == 0x051F0000:
            imm, up = w & 0xFFF, (w >> 23) & 1
            lit[(w >> 12) & 0xF] = pc + 8 + (imm if up else -imm)
        elif (w & 0x0FFF0FF0) == 0x079F0000 or (not loads_only and (w & 0x0FFF0FF0) == 0x008F0000):
            rm = w & 0xF
            if rm in lit and ((w >> 16) & 0xF) == 0xF:
                out.append((pc + 8 + u.u32(lit[rm])) & 0xFFFFFFFF)
    return out


def derive_choreo_stop(u, choreo):
    """The Choreographer VSYNC *stop* function is the enable function's twin:
    same API-level gate (its first `bl`), same lazily-built Choreographer
    singleton global, different tail call. It builds that singleton too -- and
    the construction blocks on a Java Looper that does not exist here -- so it
    is patched to `ret` alongside the enable."""
    first_bl = None
    for k in range(8):
        w = u.u32(choreo + 4 * k)
        if is_bl(w):
            first_bl = bl_target(w, choreo + 4 * k)
            break
    singles = set(_pcrel_globals(u, choreo, 40, loads_only=True)) or \
        set(e[3] for e in u.track_globals(choreo, 40) if e[0] == "load")
    if first_bl is None or not singles:
        return None
    cands = set()
    for site, _ in u.bl_targets_to([first_bl]):
        fn = u.func_start(site)
        if fn is None or fn == choreo or site - fn > 16:
            continue
        theirs = set(_pcrel_globals(u, fn, 40, loads_only=True)) or \
            set(e[3] for e in u.track_globals(fn, 40) if e[0] == "load")
        if singles & theirs:
            cands.add(fn)
    return cands.pop() if len(cands) == 1 else None


# ---------------------------------------------------------------------------
def derive_gc(m):
    """Mono bdwgc thread-suspend: the tkill wrapper, GC_suspend_all /
    GC_restart_all, and the two signal numbers they pass. On Horizon no signal
    is delivered, so these parameterise the coop-suspend flag or the fallback
    bridge (see mono_rt.c)."""
    tk = m.plt_sites("tkill")
    if not tk:
        return None
    wrap = m.func_start(m.bl_targets_to(tk)[0][0])
    callers = sorted({m.func_start(c) for c, _ in m.bl_targets_to([wrap])})

    def sig_before(fn):
        # the mov r1,#imm feeding the wrapper's 2nd arg
        for c, _ in m.bl_targets_to([wrap]):
            if m.func_start(c) != fn:
                continue
            for k in range(1, 10):
                w = m.u32(c - 4 * k)
                if (w & 0x0FFFF000) == 0x03A01000:   # mov r1,#imm8
                    return w & 0xFF
        return None

    info = dict(wrapper=wrap, funcs=[])
    for fn in callers:
        info["funcs"].append((fn, sig_before(fn), m.guard(fn)))
    # sem_wait ack loop (restart side waits on the per-thread ack semaphore)
    sw = m.plt_sites("sem_wait")
    info["sem_wait_sites"] = sw
    return info


# ---------------------------------------------------------------------------
def derive_mono(m):
    """Mono internals the runtime layer needs beyond the 4 GC offsets.

    mono_arch_flush_icache: the only `svc #0` in libmono, reached with
      r7 = 0xF0002 (__ARM_NR_cacheflush). A Linux syscall is not a Horizon SVC,
      so the function is hooked to the port's cache maintenance. It is a leaf
      (no lr in its push), so its start is the nearest `push` above the svc.

    GC_suspend_handler: the function that calls pthread_self, compares
      me->stop_info.last_stop_count against GC_stop_count, stores its sp into
      me->stop_info.stack_ptr, sem_posts GC_suspend_ack_sem and sigsuspends.
      Everything the bridge must reproduce on the thread's behalf is read out
      of that code: the GOT slots of GC_stop_count and GC_suspend_ack_sem,
      GC_lookup_thread, and the three GC_thread field offsets."""
    out = {}
    base, d = m.text()
    svcs = [base + o for o in range(0, len(d), 4) if struct.unpack_from("<I", d, o)[0] == 0xEF000000]
    for sv in svcs:
        fn = None
        for k in range(1, 40):
            w = m.u32(sv - 4 * k)
            if (w & 0xFFFF0000) == 0xE92D0000:       # push {...}
                fn = sv - 4 * k
                break
        body = [m.u32(fn + 4 * k) for k in range((sv - fn) // 4)] if fn else []
        if fn and any((w & 0xFFFF0FFF) == 0xE340000F for w in body) and \
           any((w & 0xFFFF0FFF) == 0xE3A00002 for w in body):   # movt Rd,#0xf / mov Rd,#2
            out["flush"] = fn
            out["flush_guard"] = m.guard(fn)
            break

    ps = set(m.plt_sites("pthread_self"))
    sp_ = set(m.plt_sites("sem_post"))
    ss = set(m.plt_sites("sigsuspend"))
    handler = None
    for site, _ in m.bl_targets_to(ss):
        fn = m.func_start(site)
        if fn is None:
            continue
        calls = [(fn + 4 * k, bl_target(m.u32(fn + 4 * k), fn + 4 * k))
                 for k in range((site - fn) // 4 + 12) if is_bl(m.u32(fn + 4 * k))]
        tg = [t for _, t in calls]
        if any(t in ps for t in tg) and sum(1 for t in tg if t in sp_) >= 2:
            handler = fn
            break
    if handler is None:
        return out
    out["handler"] = handler
    out["handler_guard"] = m.guard(handler)

    got = None
    got_loads = []          # (pc, got offset) of `ldr r3,[pc,#i]; ldr r3,[r4,r3]`
    lookup = None
    seen_self = 0
    self_slot = None        # [fp,#-N] where pthread_self()'s result is kept
    prev = 0
    fields = {}
    lastlit = {}
    for k in range(80):
        pc = handler + 4 * k
        w = m.u32(pc)
        if (w & 0x0F7F0000) == 0x051F0000:                     # ldr Rt,[pc,#imm]
            imm, up = w & 0xFFF, (w >> 23) & 1
            lastlit[(w >> 12) & 0xF] = m.u32(pc + 8 + (imm if up else -imm))
        elif (w & 0x0FFF0FF0) == 0x008F0000 and got is None:   # add Rd,pc,Rm
            rm = w & 0xF
            if rm in lastlit:
                got = (pc + 8 + lastlit[rm]) & 0xFFFFFFFF
        elif (w & 0x0FF00FF0) == 0x07900000 and ((w >> 16) & 0xF) == 4:   # ldr Rt,[r4,Rm]
            rm = w & 0xF
            if rm in lastlit:
                off = lastlit[rm] - (1 << 32) if lastlit[rm] & 0x80000000 else lastlit[rm]
                got_loads.append((pc, off))
        elif is_bl(w):
            t = bl_target(w, pc)
            if t in ps:
                seen_self += 1
                nxt = m.u32(pc + 4)
                if self_slot is None and (nxt & 0xFFFFF000) == 0xE50B0000:   # str r0,[fp,#-N]
                    self_slot = nxt & 0xFFF
            elif t not in sp_ and t not in ss and lookup is None and self_slot is not None \
                    and prev == (0xE51B0000 | self_slot):                    # ldr r0,[fp,#-N]
                lookup = t
            elif t in sp_ and "ack_slot" not in out and got_loads:
                out["ack_slot"] = got_loads[-1][1]
        elif (w & 0x0FFF0000) == 0x05830000 or (w & 0x0FFF0000) == 0x05930000:
            # str/ldr r2,[r3,#imm] -- GC_thread field accesses
            imm = w & 0xFFF
            is_ldr = (w >> 20) & 1
            if is_ldr and imm and "last_stop" not in fields:
                fields["last_stop"] = imm
            elif not is_ldr and "last_stop" in fields and "stack_ptr" not in fields and imm != fields["last_stop"]:
                fields["stack_ptr"] = imm
            elif not is_ldr and "stack_ptr" in fields and imm not in (fields["last_stop"], fields["stack_ptr"]):
                fields.setdefault("signal", imm)
        prev = w
        if (w & 0xFFFFFFFF) == 0xE1A0F00E or ((w & 0xFFFF0000) == 0xE8BD0000 and w & 0x8000):
            break
    if got_loads:
        out["stop_slot"] = got_loads[0][1]
    out["got"] = got
    out["lookup"] = lookup
    if lookup:
        out["lookup_guard"] = m.guard(lookup)
    out["fields"] = fields
    return out


# ---------------------------------------------------------------------------
def emit(t, v, g, unity_bid, mono_bid, out, mono=None, unity_ver="?"):
    w = out.write
    w("/* GENERATED by tools/offsets/derive_offsets.py -- do not hand-edit.\n"
      " *\n"
      " *   libunity.so  Unity %s (Mono)  BuildID %s\n"
      " *   libmono.so                              BuildID %s\n"
      " *\n" % (unity_ver, unity_bid, mono_bid))
    w(" * Unity 5.6/2017.4 pace frames with the Choreographer VSYNC path (pumped,\n"
      " * not patched); Unity 5.3 has no such path (no VSYNC section below) and\n"
      " * paces with eglSwapInterval / targetFrameRate. The engine clock is\n"
      " * clock_gettime-backed and correct while awake; the Time hooks exist to\n"
      " * freeze it across sleep/HOME. The GC offsets describe Mono bdwgc's\n"
      " * tkill-based thread suspend.\n"
      " *\n"
      " * Every RVA carries the game's own leading words as a guard; consumers\n"
      " * verify before writing and SKIP on mismatch.\n"
      " */\n"
      "#ifndef DCR_OFFSETS_H\n#define DCR_OFFSETS_H\n#include <stdint.h>\n\n"
      "typedef struct { const char *name; uint32_t rva; uint32_t guard[4]; } DcrSite;\n"
      "#define DCR_NSITES(a) ((int)(sizeof(a) / sizeof((a)[0])))\n\n")
    w('#define DCR_UNITY_BUILD_ID "%s"\n#define DCR_MONO_BUILD_ID  "%s"\n\n' % (unity_bid, mono_bid))

    # ---- Time: 4 TimeManager entry points ----
    w("/* ===== TIME: 4 TimeManager entry points ===== */\n")
    for nm in ("GetTimeManager", "GetRealtimeSinceStartup", "SetTimeScale", "SetFixedDeltaTime"):
        rva, gd = t["entries"][nm]
        w("#define DCR_RVA_%-24s 0x%07xu\n" % (nm, rva))
        w("static const uint32_t DCR_GUARD_%s[4] = %s;\n" % (nm, hexguard(gd)))
    w("\n/* TimeManager field layout (Unity %s), read off the getters. */\n" % unity_ver)
    for k, off in t["fieldmap"].items():
        w("#define DCR_TM_FIELD_%-18s 0x%x\n" % (k, off))

    # ---- Time: 11 icall bindings ----
    w("\n/* ===== TIME: %d Time icall bindings (of %d declared) ===== */\n" % (len(t["hooks"]), t["N"]))
    w("static const DcrSite DCR_TIME_SITES[] = {\n")
    for nm in TIME_HOOKS:
        if nm not in t["hooks"]:
            continue
        fn, gd, note = t["hooks"][nm]
        w('  { "%s", 0x%07xu, %s },  /* %s */\n' % (nm, fn, hexguard(gd), note))
    w("};\n")

    # ---- vsync ----
    w("\n/* ===== VSYNC (\"swappy\"): pumped, Choreographer patched to ret ===== */\n")
    if v and v.get("wait"):
        w("#define DCR_RVA_WaitVSync          0x%07xu\n" % v["wait"])
        w("static const uint32_t DCR_GUARD_WaitVSync[4] = %s;\n" % hexguard(v["wait_guard"]))
        w("#define DCR_VSYNC_MUTEX            0x%07xu\n" % v["mutex"])
        w("#define DCR_VSYNC_COND             0x%07xu\n" % v["cond"])
        w("#define DCR_VSYNC_COUNTER          0x%07xu   /* int32 */\n" % v["counter"])
    if v and v.get("choreo"):
        w("#define DCR_RVA_ChoreographerEnableVSync 0x%07xu\n" % v["choreo"])
        w("static const uint32_t DCR_GUARD_ChoreographerEnableVSync[4] = %s;\n"
          % hexguard(v["choreo_guard"]))
    if v and v.get("choreo_stop"):
        w("#define DCR_RVA_ChoreographerStopVSync 0x%07xu\n" % v["choreo_stop"])
        w("static const uint32_t DCR_GUARD_ChoreographerStopVSync[4] = %s;\n"
          % hexguard(v["choreo_stop_guard"]))

    # ---- GC ----
    w("\n/* ===== GC: Mono bdwgc tkill-based thread suspend (4 offsets) ===== */\n")
    if g:
        w("#define DCR_MONO_THREAD_KILL       0x%07xu   /* tkill wrapper */\n" % g["wrapper"])
        labels = ["GC_SUSPEND", "GC_RESTART"]
        for idx, (fn, sig, gd) in enumerate(g["funcs"]):
            lab = labels[idx] if idx < len(labels) else "GC_FN%d" % idx
            w("#define DCR_MONO_%s_FN         0x%07xu\n" % (lab, fn))
            if sig is not None:
                w("#define DCR_MONO_%s_SIG        %du\n" % (lab, sig))
            w("static const uint32_t DCR_GUARD_MONO_%s[4] = %s;\n" % (lab, hexguard(gd)))
        if g.get("sem_wait_sites"):
            w("#define DCR_MONO_GC_ACK_SEM_WAIT   0x%07xu   /* restart ack sem_wait */\n"
              % min(g["sem_wait_sites"]))

    # ---- Mono internals (libmono RVAs) ----
    if mono:
        w("\n/* ===== MONO internals (libmono RVAs) ===== */\n")
        if mono.get("flush"):
            w("#define DCR_MONO_RVA_FlushIcache   0x%07xu   /* mono_arch_flush_icache: Linux cacheflush svc */\n" % mono["flush"])
            w("static const uint32_t DCR_MONO_GUARD_FlushIcache[4] = %s;\n" % hexguard(mono["flush_guard"]))
        if mono.get("handler"):
            w("#define DCR_MONO_RVA_SuspendHandler 0x%07xu  /* GC_suspend_handler (reference) */\n" % mono["handler"])
            w("static const uint32_t DCR_MONO_GUARD_SuspendHandler[4] = %s;\n" % hexguard(mono["handler_guard"]))
        if mono.get("lookup"):
            w("#define DCR_MONO_RVA_LookupThread  0x%07xu   /* GC_lookup_thread(pthread_t) */\n" % mono["lookup"])
            w("static const uint32_t DCR_MONO_GUARD_LookupThread[4] = %s;\n" % hexguard(mono["lookup_guard"]))
        if mono.get("got") is not None and "stop_slot" in mono and "ack_slot" in mono:
            w("#define DCR_MONO_GOT_StopCount     0x%07xu   /* GOT slot -> &GC_stop_count */\n"
              % ((mono["got"] + mono["stop_slot"]) & 0xFFFFFFFF))
            w("#define DCR_MONO_GOT_AckSem        0x%07xu   /* GOT slot -> &GC_suspend_ack_sem */\n"
              % ((mono["got"] + mono["ack_slot"]) & 0xFFFFFFFF))
        f = mono.get("fields", {})
        for k, nm in (("signal", "SIGNAL"), ("last_stop", "LAST_STOP"), ("stack_ptr", "STACK_PTR")):
            if k in f:
                w("#define DCR_MONO_GCT_%-10s 0x%x   /* GC_thread->stop_info field */\n" % (nm, f[k]))

    w("\n#endif /* DCR_OFFSETS_H */\n")


def unity_version(u):
    """The engine's own version string, e.g. "5.6.4f1" (next to its changeset)."""
    import re
    m = re.search(rb"\0(\d+\.\d+\.\d+[abfp]\d+) \([0-9a-f]{12}\)\0", u.raw)
    return m.group(1).decode() if m else "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("libdir", help="path to lib/armeabi-v7a")
    ap.add_argument("-o", "--out", default=None)
    a = ap.parse_args()
    up = os.path.join(a.libdir, "libunity.so")
    mp = os.path.join(a.libdir, "libmono.so")
    u, m = Arm32(up), Arm32(mp)

    def bid(e):
        for sec in e.e.iter_sections():
            if sec.name == ".note.gnu.build-id":
                d = sec.data()
                return d[16:].hex()
        return "?"

    t = derive_time(u)
    v = derive_vsync(u)
    g = derive_gc(m)
    mono = derive_mono(m)

    sys.stderr.write("== TIME (%d hooks + 4 entries) ==\n" % len(t["hooks"]))
    for nm, (fn, _, note) in t["hooks"].items():
        sys.stderr.write("  %-28s 0x%x  %s\n" % (nm, fn, note))
    for nm, (rva, _) in t["entries"].items():
        sys.stderr.write("  %-28s 0x%x\n" % (nm, rva))
    sys.stderr.write("== VSYNC ==\n  %s\n" % {k: hex(v[k]) for k in ("wait", "mutex", "cond", "counter", "choreo", "choreo_stop") if v and k in v})
    sys.stderr.write("== MONO ==\n  %s\n" % {k: (hex(x) if isinstance(x, int) else x) for k, x in mono.items() if not k.endswith("guard")})
    sys.stderr.write("== GC ==\n")
    if g:
        for fn, sig, _ in g["funcs"]:
            sys.stderr.write("  fn 0x%x  signal=%s\n" % (fn, sig))

    dst = a.out or os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))), "source", "dcr_offsets.h")
    with open(dst, "w") as fo:
        emit(t, v, g, bid(u), bid(m), fo, mono, unity_version(u))
    sys.stderr.write("\nwrote %s\n" % dst)


if __name__ == "__main__":
    main()
