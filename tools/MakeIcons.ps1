param([string]$Preview = "")
# Generates the extra icons stored in QuickMonForever.scr / QuickMonForever-32.exe (System.Drawing only,
# no external tools). Each icon is drawn natively at 256/64/48/32/24/16 px and packed as PNG
# entries into src\icons\<name>.ico. Also writes src\icons.rc2, which lists them in icon-index
# order (resource ID = 100 + index). Index 0, the main icon, is src\QuickMonForever.ico (not generated).
#
#   powershell -ExecutionPolicy Bypass -File tools\MakeIcons.ps1 [-Preview <sheet.png>]
#
# Order: every square icon (chart, then the 5 settings designs, 10 colors each), then the same
# with rounded corners (the chart in a gear is round already, so it is not repeated).
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$icoDir = Join-Path $root 'src\icons'
New-Item -ItemType Directory -Force $icoDir | Out-Null

function C([string]$hex, [int]$a = 255) { $c = [Drawing.ColorTranslator]::FromHtml($hex); [Drawing.Color]::FromArgb($a, $c) }
function B([string]$hex, [int]$a = 255) { New-Object Drawing.SolidBrush (C $hex $a) }
function P([string]$hex, [float]$w) { $p = New-Object Drawing.Pen((C $hex), $w); $p.LineJoin = 'Round'; $p.StartCap = 'Round'; $p.EndCap = 'Round'; $p }
function RR([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
  $p = New-Object Drawing.Drawing2D.GraphicsPath; $d = $r * 2
  $p.AddArc($x, $y, $d, $d, 180, 90); $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
  $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90); $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
  $p.CloseFigure(); $p
}
function Dim([string]$hex, [double]$f) { $c = C $hex; '#{0:X2}{1:X2}{2:X2}' -f [int]($c.R * $f), [int]($c.G * $f), [int]($c.B * $f) }

# Icon colors: the saver's Grid Color list (kGridColors in QuickMonForever.cpp, grid line color and
# frame color), with Green first (the main icon's color) and White added after Light Gray.
$colors = @(
  @{ Key = 'Green';     Name = 'Green';      grid = '#1E552D'; edge = '#3CAA5A' },
  @{ Key = 'Gray';      Name = 'Gray';       grid = '#505050'; edge = '#C0C0C0' },
  @{ Key = 'DarkGray';  Name = 'Dark Gray';  grid = '#303030'; edge = '#808080' },
  @{ Key = 'LightGray'; Name = 'Light Gray'; grid = '#787878'; edge = '#DCDCDC' },
  @{ Key = 'White';     Name = 'White';      grid = '#A0A0A0'; edge = '#FFFFFF' },
  @{ Key = 'Blue';      Name = 'Blue';       grid = '#233778'; edge = '#5078E6' },
  @{ Key = 'Teal';      Name = 'Teal';       grid = '#005555'; edge = '#00AAAA' },
  @{ Key = 'Amber';     Name = 'Amber';      grid = '#6E4B00'; edge = '#DC9600' },
  @{ Key = 'Red';       Name = 'Red';        grid = '#691E1E'; edge = '#D23C3C' },
  @{ Key = 'Purple';    Name = 'Purple';     grid = '#4B2D6E'; edge = '#965ADC' }
)
foreach ($c in $colors) { $c.gridSmall = if ($c.Key -eq 'Green') { '#174425' } else { Dim $c.grid 0.78 } }

# The saver's palette: green, red, yellow, cyan, blue, magenta (y: 0 = bottom, 100 = top).
$series = @(
  @{ c = '#3CDC3C'; v = 22, 30, 26, 44, 38, 58, 50, 70, 64 },
  @{ c = '#FF4646'; v = 48, 62, 52, 74, 64, 84, 70, 88, 78 },
  @{ c = '#FFE63C'; v = 64, 50, 60, 40, 54, 34, 46, 28, 40 },
  @{ c = '#3CE6E6'; v = 86, 80, 88, 76, 84, 70, 78, 66, 72 },
  @{ c = '#508CFF'; v = 8, 10, 13, 10, 15, 12, 18, 14, 20 },
  @{ c = '#FF50FF'; v = 36, 40, 34, 30, 36, 28, 32, 24, 30 }
)

# The chart (the main icon's look) in the rectangle (ox, oy, w, h). Square: whole-pixel frame.
# Rounded: clipped to rounded corners with a thin frame along them.
function Chart($g, [float]$ox, [float]$oy, [float]$w, [float]$h, [int]$size, $col, [bool]$edge, [bool]$round) {
  $rad = [Math]::Max(2, $size * 0.11)
  if ($round) { $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'Half'; $g.SetClip((RR $ox $oy $w $h $rad)) }
  $g.SmoothingMode = 'None'; $g.PixelOffsetMode = 'None'
  $g.FillRectangle((B '#000000'), $ox, $oy, $w, $h)
  $div = [Math]::Max(3, [Math]::Min(10, [int][Math]::Floor([Math]::Min($w, $h) / 5)))
  $gc = if ($size -le 32) { $col.gridSmall } else { $col.grid }
  $gp = New-Object Drawing.Pen((C $gc), 1)
  for ($i = 1; $i -lt $div; $i++) {
    $x = [int]($ox + [Math]::Round($i * $w / $div)); $y = [int]($oy + [Math]::Round($i * $h / $div))
    $g.DrawLine($gp, $ox, $y, $ox + $w, $y); $g.DrawLine($gp, $x, $oy, $x, $oy + $h)
  }
  $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'Half'
  $use = if ($size -le 24) { $series[0, 1, 3] } elseif ($size -le 32) { $series[0..3] } else { $series }
  $lw = [Math]::Max(1.0, $size / 110.0); $m = [Math]::Max(1.0, $h * 0.04)
  foreach ($s in $use) {
    $pts = New-Object 'System.Collections.Generic.List[Drawing.PointF]'
    for ($i = 0; $i -lt $s.v.Count; $i++) { $pts.Add((New-Object Drawing.PointF ($ox + $i * $w / ($s.v.Count - 1)), ($oy + $h - $m - $s.v[$i] / 100.0 * ($h - 2 * $m)))) }
    $g.DrawLines((P $s.c $lw), $pts.ToArray())
  }
  if ($round) {
    $g.ResetClip()
    if ($edge) { $ew = [Math]::Max(1, $size / 128.0); $g.DrawPath((New-Object Drawing.Pen((C $col.edge), $ew)), (RR ($ox + $ew / 2) ($oy + $ew / 2) ($w - $ew) ($h - $ew) $rad)) }
  } elseif ($edge) {
    $bw = [int][Math]::Max(1, [Math]::Round($size / 128.0)); $eb = B $col.edge
    $g.SmoothingMode = 'None'; $g.PixelOffsetMode = 'None'
    $g.FillRectangle($eb, $ox, $oy, $w, $bw); $g.FillRectangle($eb, $ox, $oy + $h - $bw, $w, $bw)
    $g.FillRectangle($eb, $ox, $oy, $bw, $h); $g.FillRectangle($eb, $ox + $w - $bw, $oy, $bw, $h)
    $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'Half'
  }
}

function GearPath([float]$cx, [float]$cy, [float]$R, [int]$teeth, [float]$holeFrac) {
  $p = New-Object Drawing.Drawing2D.GraphicsPath; $p.FillMode = 'Alternate'
  $pts = New-Object 'System.Collections.Generic.List[Drawing.PointF]'
  $Ri = $R * 0.76; $step = 2 * [Math]::PI / $teeth
  for ($t = 0; $t -lt $teeth; $t++) {
    $a = $t * $step - [Math]::PI / 2
    foreach ($pair in @(@(-0.30, $Ri), @(-0.20, $R), @(0.20, $R), @(0.30, $Ri))) {
      $ang = $a + $pair[0] * $step; $pts.Add((New-Object Drawing.PointF ($cx + $pair[1] * [Math]::Cos($ang)), ($cy + $pair[1] * [Math]::Sin($ang))))
    }
    foreach ($f in 0.45, 0.55) { $ang = $a + $f * $step; $pts.Add((New-Object Drawing.PointF ($cx + $Ri * [Math]::Cos($ang)), ($cy + $Ri * [Math]::Sin($ang)))) }
  }
  $p.AddPolygon($pts.ToArray())
  if ($holeFrac -gt 0) { $hr = $R * $holeFrac; $p.AddEllipse($cx - $hr, $cy - $hr, 2 * $hr, 2 * $hr) }
  $p
}
function GearBadge($g, [int]$size, [string]$fill, [string]$outline) {
  $R = $size * 0.30; $cx = $size - $R - $size * 0.02; $cy = $cx
  $gp = GearPath $cx $cy $R 8 0.32
  $g.DrawPath((New-Object Drawing.Pen((C $outline), [Math]::Max(1.5, $size * 0.045))), $gp)
  $g.FillPath((B $fill), $gp)
  $hr = $R * 0.32; $g.FillEllipse((B '#101010'), $cx - $hr, $cy - $hr, 2 * $hr, 2 * $hr)
}

# Designs. $s = pixel size, $col = grid color, $rd = rounded corners. Tools take the frame color.
$charts = @{ Key = 'Chart'; Name = 'Chart'; Draw = { param($g, $s, $col, $rd) Chart $g 0 0 $s $s $s $col $true $rd } }
$tools = @(
  @{ Key = 'Gear'; Name = 'White gear'; Draw = { param($g, $s, $col, $rd) Chart $g 0 0 $s $s $s $col $true $rd; GearBadge $g $s '#ECEFF3' '#101010' } },
  @{ Key = 'ColorGear'; Name = 'Colored gear'; Draw = { param($g, $s, $col, $rd) Chart $g 0 0 $s $s $s $col $true $rd; GearBadge $g $s $col.edge '#0A0A0A' } },
  @{ Key = 'Sliders'; Name = 'Sliders'; Draw = {
      param($g, $s, $col, $rd)
      Chart $g 0 0 $s $s $s $col $true $rd
      $bs = $s * 0.56; $bx = $s - $bs; $by = $s - $bs; $r = [Math]::Max(2, $bs * 0.18)
      $path = RR $bx $by $bs $bs $r
      $g.FillPath((B $col.edge), $path); $g.DrawPath((New-Object Drawing.Pen((C '#0A0A0A'), [Math]::Max(1, $s * 0.03))), $path)
      $n = if ($s -le 24) { 2 } else { 3 }; $lw = [Math]::Max(1.2, $bs * 0.07); $kr = [Math]::Max(1.6, $bs * 0.11)
      $knob = @(0.30, 0.68, 0.45)
      for ($i = 0; $i -lt $n; $i++) {
        $y = $by + $bs * ($i + 1) / ($n + 1); $x1 = $bx + $bs * 0.2; $x2 = $bx + $bs * 0.8
        $g.DrawLine((P '#FFFFFF' $lw), $x1, $y, $x2, $y)
        $kx = $x1 + ($x2 - $x1) * $knob[$i]; $g.FillEllipse((B '#FFFFFF'), $kx - $kr, $y - $kr, 2 * $kr, 2 * $kr)
        $g.DrawEllipse((New-Object Drawing.Pen((C $col.grid), [Math]::Max(1, $bs * 0.03))), $kx - $kr, $y - $kr, 2 * $kr, 2 * $kr)
      } } },
  @{ Key = 'Wrench'; Name = 'Wrench'; Draw = {
      param($g, $s, $col, $rd)
      Chart $g 0 0 $s $s $s $col $true $rd
      $L = New-Object Drawing.Bitmap($s, $s, [Drawing.Imaging.PixelFormat]::Format32bppArgb); $lg = [Drawing.Graphics]::FromImage($L)
      $lg.SmoothingMode = 'AntiAlias'; $lg.PixelOffsetMode = 'Half'
      $hw = [Math]::Max(2.4, $s * 0.11); $hx = $s * 0.70; $hy = $s * 0.55; $hr = $s * 0.17
      $x1 = $s * 0.40; $y1 = $s * 0.96; $out = [Math]::Max(1.5, $s * 0.05)
      $lg.DrawLine((P '#0A0A0A' ($hw + 2 * $out)), $x1, $y1, $hx, $hy); $lg.FillEllipse((B '#0A0A0A'), $hx - $hr - $out, $hy - $hr - $out, 2 * ($hr + $out), 2 * ($hr + $out))
      $lg.DrawLine((P $col.edge $hw), $x1, $y1, $hx, $hy); $lg.FillEllipse((B $col.edge), $hx - $hr, $hy - $hr, 2 * $hr, 2 * $hr)
      $lg.CompositingMode = 'SourceCopy'; $jw = $hr * 0.62
      $jaw = New-Object Drawing.Drawing2D.Matrix; $jaw.RotateAt(-30, (New-Object Drawing.PointF $hx, $hy))
      $lg.Transform = $jaw; $lg.FillRectangle((New-Object Drawing.SolidBrush([Drawing.Color]::Transparent)), $hx - $jw / 2, $hy - $hr - $out - 1, $jw, $hr + $out + 1)
      $lg.ResetTransform(); $lg.Dispose()
      $g.DrawImageUnscaled($L, 0, 0); $L.Dispose() } },
  @{ Key = 'ChartGear'; Name = 'Chart in a gear'; NoRound = $true; Draw = {
      param($g, $s, $col, $rd)
      $c = $s / 2; $R = $s * 0.49
      $outer = GearPath $c $c $R 8 0.0
      $g.FillPath((B $col.edge), $outer)
      $g.DrawPath((New-Object Drawing.Pen((C $col.grid), [Math]::Max(1, $s / 128.0))), $outer)
      $ir = $R * 0.66; $clip = New-Object Drawing.Drawing2D.GraphicsPath; $clip.AddEllipse($c - $ir, $c - $ir, 2 * $ir, 2 * $ir)
      $g.SetClip($clip); Chart $g ($c - $ir) ($c - $ir) (2 * $ir) (2 * $ir) $s $col $false $false; $g.ResetClip()
      $g.SmoothingMode = 'AntiAlias'; $g.DrawEllipse((New-Object Drawing.Pen((C '#0A0A0A'), [Math]::Max(1, $s * 0.025))), $c - $ir, $c - $ir, 2 * $ir, 2 * $ir) } }
)

# The icon list, in index order (index 0 = src\QuickMonForever.ico).
# Every square icon first, then the same icons with rounded corners. The square green chart is
# index 0 (src\QuickMonForever.ico), so it is not generated.
$list = New-Object System.Collections.ArrayList
foreach ($rd in $false, $true) {
  foreach ($d in @($charts) + $tools) {
    if ($rd -and $d.NoRound) { continue }
    foreach ($col in $colors) {
      if (-not $rd -and $d.Key -eq 'Chart' -and $col.Key -eq 'Green') { continue }
      [void]$list.Add(@{ D = $d; Col = $col; Round = $rd })
    }
  }
}

$sizes = 256, 64, 48, 32, 24, 16
function Render($e, [int]$size) {
  $bmp = New-Object Drawing.Bitmap($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [Drawing.Graphics]::FromImage($bmp); $g.Clear([Drawing.Color]::Transparent); $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'Half'
  & $e.D.Draw $g $size $e.Col $e.Round; $g.Dispose(); $bmp
}
function WriteIco([string]$path, $pngs) {
  $ms = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter $ms
  $w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
  $offset = 6 + 16 * $sizes.Count
  for ($i = 0; $i -lt $sizes.Count; $i++) {
    $d = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $w.Write([byte]$d); $w.Write([byte]$d); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]$pngs[$i].Length); $w.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
  }
  foreach ($b in $pngs) { $w.Write($b) }
  $w.Flush(); [IO.File]::WriteAllBytes($path, $ms.ToArray())
}

$rc = New-Object System.Text.StringBuilder
[void]$rc.AppendLine('// Generated by tools\MakeIcons.ps1 - do not edit. Resource ID = 100 + icon index.')
$idx = 1; $thumbs = @()
foreach ($e in $list) {
  $shape = if ($e.Round) { 'Rounded' } else { 'Square' }
  $name = "$($e.D.Key)-$shape-$($e.Col.Key)"
  $pngs = foreach ($s in $sizes) {
    $bmp = Render $e $s; $m = New-Object IO.MemoryStream; $bmp.Save($m, [Drawing.Imaging.ImageFormat]::Png)
    if ($s -eq 48) { $thumbs += , @($idx, $bmp) } else { $bmp.Dispose() }
    , $m.ToArray()
  }
  WriteIco (Join-Path $icoDir "$name.ico") $pngs
  # The setup dialog's own icon: the white gear, square, green (IDI_SETTINGS in resource.h).
  $id = if ($name -eq 'Gear-Square-Green') { 'IDI_SETTINGS' } else { "$(100 + $idx)" }
  [void]$rc.AppendLine(('{0,-13} ICON "icons/{1}.ico"    // index {2}: {3}, {4}, {5}' -f $id, $name, $idx, $e.D.Name, $shape.ToLower(), $e.Col.Name))
  $idx++
}
[IO.File]::WriteAllText((Join-Path $root 'src\icons.rc2'), $rc.ToString())
"wrote $($list.Count) icons to $icoDir and src\icons.rc2"

if ($Preview) {
  # One row per design and shape, one column per color (index 0 = the main icon, top left),
  # with the color names across the top and the row names down the left.
  $cols = $colors.Count; $cell = 80; $rows = [Math]::Ceiling(($thumbs.Count + 1) / $cols)
  $rowNames = 'Square Plain', 'Square White Gear', 'Square Colored Gears', 'Square Colored Menu',
              'Square Colored Wrench', 'Colored Gear Only', 'Rounded Plain', 'Rounded White Gear',
              'Rounded Colored Gears', 'Rounded Colored Menu', 'Rounded Colored Wrench'
  $lw = 190; $hh = 34
  $sheet = New-Object Drawing.Bitmap(($lw + $cols * $cell), ($hh + $rows * $cell)); $sg = [Drawing.Graphics]::FromImage($sheet)
  $sg.Clear((C '#F4F4F4')); $sg.TextRenderingHint = 'AntiAliasGridFit'
  $f = New-Object Drawing.Font('Segoe UI', 8); $fh = New-Object Drawing.Font('Segoe UI Semibold', 10)
  $ink = B '#202020'; $line = New-Object Drawing.Pen((C '#D8D8D8'), 1)
  $center = New-Object Drawing.StringFormat; $center.Alignment = 'Center'; $center.LineAlignment = 'Center'
  $left = New-Object Drawing.StringFormat; $left.LineAlignment = 'Center'
  for ($c = 0; $c -lt $cols; $c++) {
    $sg.DrawString($colors[$c].Name, $fh, $ink, (New-Object Drawing.RectangleF ($lw + $c * $cell), 0, $cell, $hh), $center)
  }
  for ($r = 0; $r -lt $rows; $r++) {
    $y = $hh + $r * $cell
    $sg.DrawLine($line, 0, $y, $lw + $cols * $cell, $y)
    $sg.DrawString($rowNames[$r], $fh, $ink, (New-Object Drawing.RectangleF 14, $y, ($lw - 14), $cell), $left)
  }
  $sg.DrawLine($line, $lw - 1, 0, $lw - 1, $hh + $rows * $cell)
  $sg.TranslateTransform($lw, $hh)
  # The main icon's 48 px entry (a PNG inside the .ico; Drawing.Icon cannot read those).
  $ib = [IO.File]::ReadAllBytes((Join-Path $root 'src\QuickMonForever.ico')); $main = $null
  for ($i = 0; $i -lt [BitConverter]::ToUInt16($ib, 4); $i++) {
    $o = 6 + 16 * $i
    if ($ib[$o] -eq 48) { $len = [BitConverter]::ToInt32($ib, $o + 8); $off = [BitConverter]::ToInt32($ib, $o + 12); $main = New-Object Drawing.Bitmap((New-Object IO.MemoryStream(, $ib[$off..($off + $len - 1)]))) }
  }
  $all = @(, @(0, $main)) + $thumbs
  foreach ($t in $all) {
    $i = $t[0]
    $x = ($i % $cols) * $cell; $y = [Math]::Floor($i / $cols) * $cell
    $sg.DrawImageUnscaled($t[1], $x + 16, $y + 6)
    $sg.DrawString("$i", $f, (B '#444444'), (New-Object Drawing.RectangleF $x, ($y + 56), $cell, 18), $center)
  }
  $sheet.Save($Preview, [Drawing.Imaging.ImageFormat]::Png); "wrote $Preview"
}
