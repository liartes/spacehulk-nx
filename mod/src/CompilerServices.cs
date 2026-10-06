// CompilerServices.cs -- MethodImplAttribute, which the game's mscorlib (Unity
// strips it) does not carry. The C# compiler knows it by its full name and
// turns [MethodImpl(InternalCall)] into the method's InternalCall flag, which
// is all Mono reads: the icalls of Native (Loader.cs). MIT.
namespace System.Runtime.CompilerServices
{
    [AttributeUsage(AttributeTargets.Method | AttributeTargets.Constructor)]
    internal sealed class MethodImplAttribute : Attribute
    {
        public MethodImplAttribute(MethodImplOptions options) { }
    }

    internal enum MethodImplOptions
    {
        InternalCall = 0x1000,
    }
}
