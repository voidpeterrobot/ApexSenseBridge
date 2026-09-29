# Run from an unlisted host process. Probe XInput directly, independently of HID
# enumeration: hiding the HID collection alone does not hide the XUSB function.
param([Parameter(Mandatory=$true)][string]$Output,[switch]$RequireNoControllers)
$ErrorActionPreference='Stop'
if (-not ('Apex6XInputVisibility' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class Apex6XInputVisibility {
    [StructLayout(LayoutKind.Sequential)] public struct Pad { public ushort buttons; public byte lt,rt; public short lx,ly,rx,ry; }
    [StructLayout(LayoutKind.Sequential)] public struct State { public uint packet; public Pad pad; }
    [StructLayout(LayoutKind.Sequential)] public struct Caps { public byte type,subtype; public ushort flags; public Pad pad; public ushort left,right; }
    [StructLayout(LayoutKind.Sequential)] public struct Ex { public Caps caps; public ushort vendor,product,version,unknown; public uint unknown2; }
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] delegate uint GetState(uint slot,out State state);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)] delegate uint Query(uint reserved,uint slot,uint flags,out Ex caps);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] static extern IntPtr LoadLibraryExW(string path,IntPtr file,uint flags);
    [DllImport("kernel32.dll",CharSet=CharSet.Ansi,ExactSpelling=true)] static extern IntPtr GetProcAddress(IntPtr module,string name);
    [DllImport("kernel32.dll",EntryPoint="GetProcAddress",ExactSpelling=true)] static extern IntPtr GetOrdinal(IntPtr module,IntPtr ordinal);
    [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr module);
    public sealed class Slot { public uint slot,status; public ushort vendor,product; }
    public static Slot[] Read() {
        if(Marshal.SizeOf(typeof(Ex))!=32)throw new Exception("XInput identity ABI mismatch");
        var dll=LoadLibraryExW("xinput1_4.dll",IntPtr.Zero,0x800);
        if(dll==IntPtr.Zero)throw new Exception("System XInput library unavailable");
        try {
            var statePtr=GetProcAddress(dll,"XInputGetState");var queryPtr=GetOrdinal(dll,new IntPtr(108));
            if(statePtr==IntPtr.Zero||queryPtr==IntPtr.Zero)throw new Exception("XInput identity functions unavailable");
            var get=(GetState)Marshal.GetDelegateForFunctionPointer(statePtr,typeof(GetState));
            var query=(Query)Marshal.GetDelegateForFunctionPointer(queryPtr,typeof(Query));
            var result=new List<Slot>();
            for(uint i=0;i<4;i++) {
                State state;var item=new Slot{slot=i,status=get(i,out state)};
                if(item.status==0){Ex caps;if(query(1,i,0,out caps)!=0&&query(0,i,0,out caps)!=0)throw new Exception("Connected XInput device identity unavailable");item.vendor=caps.vendor;item.product=caps.product;}
                else if(item.status!=1167)throw new Exception("Unexpected XInput visibility error");
                result.Add(item);
            }
            return result.ToArray();
        } finally {FreeLibrary(dll);}
    }
}
'@
}
$slots=@([Apex6XInputVisibility]::Read())
$slots | ConvertTo-Json | Set-Content -LiteralPath $Output -Encoding UTF8
if ($RequireNoControllers -and @($slots | Where-Object {$_.status -eq 0}).Count) { throw 'An XInput controller remains visible; close its emulator or disconnect it before the isolated DualSense session.' }
if (@($slots | Where-Object {$_.status -eq 0 -and $_.vendor -eq 0x37d7 -and $_.product -eq 0x2502}).Count) { throw 'Physical Apex6 is still accessible through XInput.' }
