# Розбір ROM-образів Symbian: заголовок, дизасемблювання, резольв import-стубів.
#
# ROM-образ = TRomImageHeader (120 байтів) + код. Це НЕ E32ImageHeader.
# Усі 32-бітні константи беруться через [Convert]::ToUInt32(hex,16): PowerShell
# парсить 8-значний hex-літерал як Int32 і мовчки робить його від'ємним.
param(
    [Parameter(Mandatory = $true)][string]$Module,   # ім'я файлу в sys\bin
    [switch]$Disasm,
    [switch]$Imports
)
$ErrorActionPreference = 'Stop'
$ROM = "D:\VsCodeProjects\SymbianProjects\eka2l1\data\drives\z\rm-320\sys\bin"
$OUT = "D:\VsCodeProjects\SymbianProjects\SymbianOverclock\out\asm"
$OD  = "D:\VsCodeProjects\SymbianProjects\toolchain\CSL_Arm_Toolchain\bin\arm-none-symbianelf-objdump.exe"
$HDR = 120
function U([string]$h) { [Convert]::ToUInt32($h, 16) }
$STUB = U "E51FF004"     # ldr pc,[pc,#-4]

New-Item -ItemType Directory -Force $OUT | Out-Null
$b  = [System.IO.File]::ReadAllBytes("$ROM\$Module")
$ca = [BitConverter]::ToUInt32($b, 20)
$cs = [BitConverter]::ToUInt32($b, 28)
Write-Host ("{0}: codeAddr=0x{1:X8} codeSize={2} exports={3} file={4}" -f `
    $Module, $ca, $cs, [BitConverter]::ToUInt32($b, 60), $b.Length)

if ($Disasm) {
    $bin = "$OUT\$Module.bin"
    [System.IO.File]::WriteAllBytes($bin, $b[$HDR..($b.Length - 1)])
    & $OD -D -b binary -m arm ("--adjust-vma=0x{0:X}" -f $ca) $bin > "$OUT\$Module.asm" 2>&1
    Write-Host ("  -> {0}\{1}.asm" -f $OUT, $Module)
}

if ($Imports) {
    # карта модулів для резольву цілей
    $map = New-Object System.Collections.ArrayList
    Get-ChildItem $ROM -File | ForEach-Object {
        try {
            $mb = [System.IO.File]::ReadAllBytes($_.FullName)
            if ($mb.Length -lt 128) { return }
            $u1 = [BitConverter]::ToUInt32($mb, 0)
            if ($u1 -ne (U "10000079") -and $u1 -ne (U "1000007A")) { return }
            $l = [BitConverter]::ToUInt32($mb, 20); $s = [BitConverter]::ToUInt32($mb, 28)
            if ($l -lt (U "80000000") -or $s -eq 0 -or $s -gt 4000000) { return }
            [void]$map.Add([pscustomobject]@{ Name = $_.Name; Lo = $l; Hi = [uint32]($l + $s) })
        } catch { }
    }
    Write-Host "  import-стуби:"
    for ($i = 0; $i -le $b.Length - 8; $i += 4) {
        if ([BitConverter]::ToUInt32($b, $i) -ne $STUB) { continue }
        $t  = [BitConverter]::ToUInt32($b, $i + 4)
        $va = [uint32]($ca + $i - $HDR)
        $m  = $map | Where-Object { $t -ge $_.Lo -and $t -lt $_.Hi } | Select-Object -First 1
        $own = if ($m) { "{0} +0x{1:X}" -f $m.Name, ($t - $m.Lo) } else { "<не резольвиться>" }
        Write-Host ("    stub@0x{0:X8} -> 0x{1:X8}  {2}" -f $va, $t, $own)
    }
}
