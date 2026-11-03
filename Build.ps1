param([string]$SmartwatchRoot, [string]$IdeRoot)
$ErrorActionPreference='Stop'
$bundled=Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
$python=if(Test-Path -LiteralPath $bundled){$bundled}else{(Get-Command python -ErrorAction Stop).Source}
$buildArgs=@((Join-Path $PSScriptRoot 'build.py'))
if($SmartwatchRoot){$buildArgs+=@('--smartwatch-root',$SmartwatchRoot)}
if($IdeRoot){$buildArgs+=@('--ide-root',$IdeRoot)}
& $python @buildArgs
if($LASTEXITCODE){throw 'Firmware build failed'}
