// FastSerialize.cs -- the game's SerializationHelper.Serialize/Deserialize,
// the same data written and read, without the reflection and exceptions
// they spend on every object.
//
// The model (undo snapshots, saves, undo itself) goes through these for
// each of its objects. Per object, the game's versions list the type's
// fields and ask each field twice for its attributes (two arrays
// allocated), call Enum.IsDefined on every field -- an ArgumentException
// thrown and caught for each one that is not an enum -- and, reading back,
// look for "<field>:null" and "<field>:enum" by catching a
// SerializationException for each one absent. On the Switch's Mono that was
// 250-550 ms a snapshot and a GC every two seconds from the garbage
// (hardware, 2026-10-09).
//
// Here: each type's fields, their attribute verdict and their entry names
// are worked out once; Enum.IsDefined only for enum fields; reading checks
// which entries the SerializationInfo has instead of throwing. Same names,
// same order, same values: saves are interchangeable with the game's own
// code. MIT.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.Serialization;
using TXEngine.NetworkedCombatSystem;

namespace DcrMod
{
    public static class FastSerialize
    {
        sealed class Field
        {
            public FieldInfo info;
            public Type type;
            public bool isEnum, isArrayList, isHashtable;
            public string name, nameNull, nameKeys, nameVals, nameEnum;
        }

        static readonly Dictionary<Type, Field[]> cache = new Dictionary<Type, Field[]>();
        static readonly object gate = new object();

        public static void Install()
        {
            Type h = typeof(SerializationHelper);
            Type[] sig = { typeof(object), typeof(SerializationInfo), typeof(StreamingContext) };
            const BindingFlags S = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
            Hook(h.GetMethod("Serialize", S, null, sig, null), typeof(FastSerialize).GetMethod("Serialize", S, null, sig, null));
            Hook(h.GetMethod("Deserialize", S, null, sig, null), typeof(FastSerialize).GetMethod("Deserialize", S, null, sig, null));
        }

        static void Hook(MethodInfo target, MethodInfo repl)
        {
            if (target == null || repl == null)
            {
                Native.Log("FastSerialize: " + (target == null ? "SerializationHelper method" : "replacement") + " not found");
                return;
            }
            int r = Native.Detour(target.MethodHandle.Value, repl.MethodHandle.Value, IntPtr.Zero);
            Native.Log("FastSerialize: SerializationHelper." + target.Name + (r == 0 ? " replaced" : " not hooked (" + r + ")"));
        }

        // the fields the game's code serializes, in its order
        static Field[] Fields(Type t)
        {
            Field[] f;
            lock (gate)
                if (cache.TryGetValue(t, out f))
                    return f;
            var list = new List<Field>();
            foreach (FieldInfo fi in t.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
            {
                if (fi.GetCustomAttributes(typeof(NonSerializedAttribute), true).Length > 0 ||
                    fi.GetCustomAttributes(typeof(CustomSerializedAttribute), true).Length > 0)
                    continue;
                list.Add(new Field
                {
                    info = fi,
                    type = fi.FieldType,
                    isEnum = fi.FieldType.IsEnum,
                    isArrayList = fi.FieldType == typeof(ArrayList),
                    isHashtable = fi.FieldType == typeof(Hashtable),
                    name = fi.Name,
                    nameNull = fi.Name + ":null",
                    nameKeys = fi.Name + ":keys",
                    nameVals = fi.Name + ":vals",
                    nameEnum = fi.Name + ":enum",
                });
            }
            f = list.ToArray();
            lock (gate)
                cache[t] = f;
            return f;
        }

        static void Serialize(object obj, SerializationInfo info, StreamingContext context)
        {
            foreach (Field f in Fields(obj.GetType()))
            {
                object value = f.info.GetValue(obj);
                if (value == null)
                {
                    info.AddValue(f.nameNull, null);
                    continue;
                }
                var al = value as ArrayList;
                if (al != null)
                {
                    info.AddValue(f.name, al.ToArray());
                    continue;
                }
                var ht = value as Hashtable;
                if (ht != null)
                {
                    object[] keys = new object[ht.Keys.Count], vals = new object[ht.Keys.Count];
                    int n = 0;
                    foreach (object k in ht.Keys)
                    {
                        keys[n] = k;
                        vals[n] = ht[k];
                        n++;
                    }
                    info.AddValue(f.nameKeys, keys);
                    info.AddValue(f.nameVals, vals);
                    continue;
                }
                // the game: Enum.IsDefined(field type, value), its
                // ArgumentException (not an enum) caught
                if (f.isEnum)
                {
                    bool defined = false;
                    try { defined = Enum.IsDefined(f.type, value); }
                    catch (ArgumentException) { }
                    if (defined)
                    {
                        info.AddValue(f.nameEnum, value.ToString());
                        continue;
                    }
                }
                info.AddValue(f.name, value);
            }
        }

        // GetValue of an entry known to be there; a SerializationException
        // all the same counts as absent, as in the game's TryGetValue
        static bool Get(SerializationInfo info, Dictionary<string, bool> has, string name, Type type, out object val)
        {
            val = null;
            if (!has.ContainsKey(name))
                return false;
            try
            {
                val = info.GetValue(name, type);
                return true;
            }
            catch (SerializationException)
            {
                return false;
            }
        }

        static void Deserialize(object obj, SerializationInfo info, StreamingContext context)
        {
            var has = new Dictionary<string, bool>(info.MemberCount);
            foreach (SerializationEntry e in info)
                has[e.Name] = true;
            foreach (Field f in Fields(obj.GetType()))
            {
                object val;
                if (Get(info, has, f.nameNull, typeof(object), out val) && val == null)
                {
                    f.info.SetValue(obj, null);
                    continue;
                }
                if (f.isArrayList)
                {
                    if (Get(info, has, f.name, typeof(object[]), out val))
                        f.info.SetValue(obj, new ArrayList((object[])val));
                    continue;
                }
                if (f.isHashtable)
                {
                    object keys, vals;
                    if (Get(info, has, f.nameKeys, typeof(object[]), out keys))
                    {
                        Get(info, has, f.nameVals, typeof(object[]), out vals);
                        object[] k = (object[])keys, v = (object[])vals;
                        var table = new Hashtable();
                        for (int j = 0; j < k.Length; j++)
                            table[k[j]] = v[j];
                        f.info.SetValue(obj, table);
                    }
                    continue;
                }
                if (Get(info, has, f.nameEnum, typeof(string), out val) && val != null)
                {
                    f.info.SetValue(obj, Enum.Parse(f.type, (string)val));
                    continue;
                }
                if (Get(info, has, f.name, f.type, out val))
                    f.info.SetValue(obj, val);
            }
        }
    }
}
