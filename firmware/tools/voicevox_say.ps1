# Synthesize speech with a local VOICEVOX engine and make StackChan say it via POST /api/say.
# Usage: .\tools\voicevox_say.ps1 -StackChan 192.168.1.50 -Text "こんにちは" [-Speaker 3] [-Emotion happy] [-Bubble]
# Works on Windows PowerShell 5.1 (bytes are passed through untouched to avoid encoding issues).
param(
    [Parameter(Mandatory = $true)][string]$StackChan,
    [Parameter(Mandatory = $true)][string]$Text,
    [int]$Speaker = 3,
    [string]$Emotion = "happy",
    [switch]$Bubble,
    [string]$Engine = "http://127.0.0.1:50021"
)
$ErrorActionPreference = "Stop"

$enc = [uri]::EscapeDataString($Text)

$query = Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$Engine/audio_query?speaker=$Speaker&text=$enc"
$queryBytes = $query.RawContentStream.ToArray()

$wav = Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$Engine/synthesis?speaker=$Speaker" `
    -ContentType "application/json" -Body $queryBytes
$wavBytes = $wav.RawContentStream.ToArray()

$url = "http://$StackChan/api/say?emotion=$Emotion"
if ($Bubble) { $url += "&text=$enc" }
$res = Invoke-WebRequest -UseBasicParsing -Method Post -Uri $url -ContentType "audio/wav" -Body $wavBytes -TimeoutSec 120
$res.Content
