$ErrorActionPreference = 'Stop'
$collectorPath = Join-Path $PSScriptRoot '..\windows\Collect-MagicTrackpadDiagnostics.ps1'
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Resolve-Path -LiteralPath $collectorPath).Path, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw 'Collector parse failed.' }
$definition = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -eq 'Protect-DiagnosticText'
}, $true)
if ($null -eq $definition) { throw 'Redaction function missing.' }
# Load only the pure text sanitizer; do not start device/event collection.
. ([ScriptBlock]::Create($definition.Extent.Text))
$IncludeSensitiveIdentifiers = $false
$fakeIdentifier = '{12345678-1234-1234-1234-123456789abc}\MagicTrackpadRawPdo\8&example&1&0324-1-ABCDEF123456'
$plain = Protect-DiagnosticText ('Children: ' + $fakeIdentifier)
if ($plain -match 'ABCDEF123456|8&example') { throw 'Plain raw PDO identifier was not redacted.' }
$json = @{ parent = $fakeIdentifier; hardwareId = 'HID\VID_004C&PID_0324&Col01';
    bthInstance = 'BTHENUM\{12345678-1234-1234-1234-123456789abc}_VID&0001004C_PID&0324\8&private-suffix' } | ConvertTo-Json
$decoded = Protect-DiagnosticText $json | ConvertFrom-Json
if ($decoded.parent -match 'ABCDEF123456|8&example') { throw 'JSON raw PDO identifier was not redacted.' }
if ($decoded.hardwareId -ne 'HID\VID_004C&PID_0324&Col01') { throw 'Hardware ID was damaged.' }
if ($decoded.bthInstance -ne 'DEVICE_INSTANCE_REDACTED') { throw 'BTH instance was not redacted.' }
$IncludeSensitiveIdentifiers = $true
if ((Protect-DiagnosticText $fakeIdentifier) -ne $fakeIdentifier) { throw 'Explicit identifier option was ignored.' }
Write-Host 'Diagnostic redaction tests passed (text, JSON, hardware ID, explicit opt-in).'
