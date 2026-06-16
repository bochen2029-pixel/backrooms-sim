# vision_probe.ps1 — DE-RISK EXPERIMENT for the VLM-vision Director (docs/TODO_director_vision.md).
# Sends real rendered player-POV PNGs to the LOCAL Qwen-VL via keel (:7071), using the EXACT same
# request envelope as director::keel_complete_vision (text + image_url data-URI + sovereign/scaffolding/
# think:false). Prints what the model actually SAYS about each frame, so we can judge — before building
# any live-host plumbing — whether vision-grounded narration beats the current text-stats "fortune cookie".
# This is throwaway experiment tooling; it changes nothing in the game.
param(
  [string]$Url = "http://127.0.0.1:7071/v1/chat/completions",
  [int]$TimeoutSec = 90
)
$ErrorActionPreference = "Stop"

# Curated spread of real POVs dumped by `--soak --out` (skip the frame-0 warm-up shots).
$images = @(
  "C:\backrooms\runs\vision_probe\a\shot_00002.png",  # yellow wall close + room opening right
  "C:\backrooms\runs\vision_probe\a\shot_00008.png",  # open pillared room, fluorescent ceiling
  "C:\backrooms\runs\vision_probe\b\shot_00003.png",  # corridor with a central column
  "C:\backrooms\runs\vision_probe\b\shot_00006.png",
  "C:\backrooms\runs\vision_probe\b\shot_00008.png"
)

# Two candidate narration prompts (surveillance-AI framing, anti-hallucination guardrail).
$prompts = [ordered]@{
  "A_surveillance" = "You are the surveillance intelligence of an endless, abandoned facility, watching a live feed from a lone wanderer's eyes. The attached image is the current frame. Speak ONE short sentence (max 18 words) to the wanderer over the intercom about what is ACTUALLY visible in this frame -- the walls, their color, the lighting, doorways, openings, turns, columns, or anything truly present. Describe ONLY what you can see; never invent objects that are not in the image. Tone: calm, clinical, quietly menacing. Output only the spoken line, no quotes."
  "B_terse" = "Look at the attached camera frame from a wanderer lost in a vast yellow maze. In one eerie sentence addressed to them, describe only what is visible (wall colors, lighting, corridor shape, any opening or column). Do not invent anything not in the image."
}

function Ask-Vlm([string]$imgPath, [string]$prompt) {
  $bytes = [System.IO.File]::ReadAllBytes($imgPath)
  $b64 = [System.Convert]::ToBase64String($bytes)
  $body = @{
    messages = @(@{
      role = "user"
      content = @(
        @{ type = "text"; text = $prompt },
        @{ type = "image_url"; image_url = @{ url = "data:image/png;base64,$b64" } }
      )
    })
    sovereign = $true
    kind = "scaffolding"
    think = $false
  } | ConvertTo-Json -Depth 8 -Compress
  $resp = Invoke-RestMethod -Uri $Url -Method Post -ContentType "application/json" -Body $body -TimeoutSec $TimeoutSec
  $content = $resp.choices[0].message.content
  # Strip any stray <think>...</think> the model might prepend despite think:false.
  $content = [regex]::Replace($content, '(?s)<think>.*?</think>', '').Trim()
  $tier = if ($resp.keel) { $resp.keel.tier } else { "?" }
  $cost = if ($resp.keel) { $resp.keel.cost } else { "?" }
  return [pscustomobject]@{ content = $content; tier = $tier; cost = $cost }
}

$report = New-Object System.Collections.Generic.List[string]
function Emit($s) { Write-Host $s; $report.Add($s) }

Emit ("=" * 78)
Emit "VLM-VISION DIRECTOR — de-risk experiment ($(Split-Path $Url -Parent))"
Emit ("=" * 78)
foreach ($img in $images) {
  $short = $img.Replace('C:\backrooms\runs\vision_probe\','')
  Emit ""
  Emit ("### POV  $short")
  foreach ($k in $prompts.Keys) {
    try {
      $r = Ask-Vlm $img $prompts[$k]
      Emit ("  [{0}]  (tier={1}, cost={2})" -f $k, $r.tier, $r.cost)
      Emit ("      -> {0}" -f $r.content)
    } catch {
      Emit ("  [{0}]  ERROR: {1}" -f $k, $_.Exception.Message)
    }
  }
}
Emit ""
Emit ("=" * 78)
$report -join "`n" | Set-Content -Encoding UTF8 "C:\backrooms\runs\vision_probe\report.txt"
Write-Host "`n(report saved to runs\vision_probe\report.txt)"
