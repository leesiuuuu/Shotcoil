# 제출 패키지 생성.
#
#   powershell -ExecutionPolicy Bypass -File packaging\make-dist.ps1
#
# Release x64 를 리빌드하고, 실행파일과 안내문을 dist\Shotcoil\ 에 모아
# dist\Shotcoil.zip 으로 압축한다. 공모전 규정은 "압축 해제 후" 크기를 재므로
# 압축 전 폴더 크기를 합산해 1,474,560 바이트와 대조한다.

$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$limit = 1474560
$stage = Join-Path $root 'dist\Shotcoil'
$zip   = Join-Path $root 'dist\Shotcoil.zip'

$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" |
    Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild를 찾을 수 없습니다. Visual Studio 2022가 필요합니다.' }

Write-Host '== 빌드 =='
& $msbuild (Join-Path $root 'Project1\Project1.sln') `
    /t:Rebuild /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw "빌드 실패 (exit $LASTEXITCODE)" }

Write-Host '== 수집 =='
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

Copy-Item (Join-Path $root 'Project1\x64\Release\Shotcoil.exe') $stage
Copy-Item (Join-Path $PSScriptRoot 'README.txt') $stage

# 갈무리 폰트가 실행파일에 서브셋으로 내장되어 있다. OFL 1.1 조건 2는 폰트를
# 번들한 사본마다 저작권 고지와 라이선스 전문을 포함할 것을 요구한다.
$ofl = Join-Path $root 'third_party\fonts\OFL.txt'
if (Test-Path $ofl) { Copy-Item $ofl (Join-Path $stage 'LICENSE-Galmuri.txt') }

Write-Host '== 용량 =='
$files = Get-ChildItem $stage -Recurse -File
$total = ($files | Measure-Object -Property Length -Sum).Sum
foreach ($f in $files) { '{0,10:N0}  {1}' -f $f.Length, $f.Name | Write-Host }
'{0,10:N0}  = 합계' -f $total | Write-Host
'{0,10:N0}  제한' -f $limit | Write-Host

if ($total -gt $limit) {
    throw ('용량 초과: {0:N0} 바이트 초과' -f ($total - $limit))
}
'여유 {0:N0} 바이트 ({1:N0} KB), 사용률 {2:N1}%' -f `
    ($limit - $total), (($limit - $total) / 1024), ($total * 100 / $limit) | Write-Host

Write-Host '== 압축 =='
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host ('완료: {0}' -f $zip)
