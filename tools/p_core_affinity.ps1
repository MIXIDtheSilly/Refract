# Affinity mask of the cores with the highest EfficiencyClass: the P-cores on a hybrid CPU (i7-12700:
# logical processors 0-15; 16-19 are E-cores), all cores otherwise. Pinning qemu to them stops Windows
# moving the vCPU threads to E-cores, e.g. while the emulator window is minimized or hidden.
# Dot-source this file, then call Get-PCoreMask.
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class PCores {
    [DllImport("kernel32.dll")] static extern bool GetLogicalProcessorInformationEx(int relation, IntPtr buffer, ref int length);
    public static long Mask() {
        int length = 0;
        GetLogicalProcessorInformationEx(0, IntPtr.Zero, ref length);  // 0 = RelationProcessorCore
        IntPtr buffer = Marshal.AllocHGlobal(length);
        try {
            GetLogicalProcessorInformationEx(0, buffer, ref length);
            long mask = 0; int best = -1;
            for (int offset = 0; offset < length; offset += Marshal.ReadInt32(buffer + offset, 4)) {
                int efficiency = Marshal.ReadByte(buffer + offset, 9);
                long coreMask = Marshal.ReadInt64(buffer + offset, 32);  // GroupMask[0].Mask
                if (efficiency > best) { best = efficiency; mask = 0; }
                if (efficiency == best) mask |= coreMask;
            }
            return mask;
        } finally { Marshal.FreeHGlobal(buffer); }
    }
}
'@
function Get-PCoreMask { [PCores]::Mask() }
