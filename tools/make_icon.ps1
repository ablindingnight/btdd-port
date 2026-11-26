# Builds the program icon from the emblem artwork (Windows, System.Drawing; no extra tools):
#   res/art/emblem.png -> res/win/pondtopavement.ico (16, 24, 32, 48, 64, 128, 256; PNG entries)
#                      -> res/art/icon.png (256 px, the SDL window icon at run time)
# Transparent margins are cropped, then the emblem is centred on a square canvas.
#   pwsh tools/make_icon.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$src = [System.Drawing.Bitmap]::FromFile((Join-Path $root 'res/art/emblem.png'))

# bounding box of the visible pixels
$x0 = $src.Width; $y0 = $src.Height; $x1 = -1; $y1 = -1
$data = $src.LockBits((New-Object System.Drawing.Rectangle 0, 0, $src.Width, $src.Height),
    [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$bytes = New-Object byte[] ($data.Stride * $src.Height)
[System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
$src.UnlockBits($data)
for ($y = 0; $y -lt $src.Height; $y++) {
    $row = $y * $data.Stride
    for ($x = 0; $x -lt $src.Width; $x++) {
        if ($bytes[$row + 4 * $x + 3] -gt 8) {
            if ($x -lt $x0) { $x0 = $x }; if ($x -gt $x1) { $x1 = $x }
            if ($y -lt $y0) { $y0 = $y }; if ($y -gt $y1) { $y1 = $y }
        }
    }
}
$w = $x1 - $x0 + 1; $h = $y1 - $y0 + 1; $side = [Math]::Max($w, $h)

# 16 and 24 px: a tighter square on the fists and the burst (the whole disc turns to mush that small)
function Render([int]$size) {
    if ($size -le 24) { $cx = $x0 + $w * 0.5; $cy = $y0 + $h * 0.55; $sd = $side * 0.62 }
    else { $cx = $x0 + $w / 2; $cy = $y0 + $h / 2; $sd = $side }
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)
    $dst = New-Object System.Drawing.RectangleF 0, 0, $size, $size
    $g.DrawImage($src, $dst, (New-Object System.Drawing.RectangleF ($cx - $sd / 2), ($cy - $sd / 2), $sd, $sd), [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return , $ms.ToArray()
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$pngs = @(); foreach ($s in $sizes) { $pngs += , (Render $s) }
New-Item -ItemType Directory -Force (Join-Path $root 'res/win') | Out-Null
$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $out
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)    # ICONDIR
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {                                       # ICONDIRENTRY
    $s = $sizes[$i]; $b = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$b); $bw.Write([byte]$b); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$pngs[$i].Length); $bw.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
[System.IO.File]::WriteAllBytes((Join-Path $root 'res/win/pondtopavement.ico'), $out.ToArray())
[System.IO.File]::WriteAllBytes((Join-Path $root 'res/art/icon.png'), $pngs[$sizes.Count - 1])
$src.Dispose()
"icon: crop ${w}x${h} at ($x0,$y0), sizes $($sizes -join ',') -> res/win/pondtopavement.ico, res/art/icon.png"
