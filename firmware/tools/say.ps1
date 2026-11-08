# Make StackChan say text with its cloud VOICEVOX settings (no local VOICEVOX needed).
# Usage: .\tools\say.ps1 -StackChan 192.168.1.50 -Text "こんにちは" [-Emotion happy]
param(
    [Parameter(Mandatory = $true)][string]$StackChan,
    [Parameter(Mandatory = $true)][string]$Text,
    [string]$Emotion = "happy"
)
$ErrorActionPreference = "Stop"

$enc = [uri]::EscapeDataString($Text)
$res = Invoke-WebRequest -UseBasicParsing -Method Post -Uri "http://$StackChan/api/say?emotion=$Emotion&text=$enc" -TimeoutSec 120
$res.Content
