using System;
using System.Runtime.InteropServices;

namespace OfflinePdf;

internal enum Phase : uint { Idle, Loading, ReadingImage, Translating, Ready, Empty, Error, Password }

internal static class Core
{
    [StructLayout(LayoutKind.Sequential)]
    internal readonly struct Bytes
    {
        readonly IntPtr data;
        readonly nuint length;
        public string Text() => length == 0 ? "" : Marshal.PtrToStringUTF8(data, checked((int)length))!;
        public byte[] Copy()
        {
            var bytes = new byte[checked((int)length)];
            if (bytes.Length != 0) Marshal.Copy(data, bytes, 0, bytes.Length);
            return bytes;
        }
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct View
    {
        public ulong Revision, ImageId;
        public uint Page, Count;
        public Phase Phase;
        public Bytes Source, Translation, Message, Png;
    }
    const string Dll = "pdf_reader";
    [DllImport(Dll)] internal static extern IntPtr reader_new();
    [DllImport(Dll)] internal static extern void reader_free(IntPtr reader);
    [DllImport(Dll)] internal static extern void reader_open(IntPtr reader, [MarshalAs(UnmanagedType.LPUTF8Str)] string path, [MarshalAs(UnmanagedType.LPUTF8Str)] string password);
    [DllImport(Dll)] internal static extern void reader_page(IntPtr reader, uint page);
    [DllImport(Dll)] internal static extern void reader_find(IntPtr reader, [MarshalAs(UnmanagedType.LPUTF8Str)] string query);
    [DllImport(Dll)] internal static extern IntPtr reader_poll(IntPtr reader, ulong revision, ulong image, out View view);
    [DllImport(Dll)] internal static extern void reader_snapshot_free(IntPtr snapshot);
}
