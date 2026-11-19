param(
    [switch]$RunTests
)

$ErrorActionPreference = 'Stop'
$sourceDir = $PSScriptRoot
$labDir = Split-Path -Parent $sourceDir
$buildDir = Join-Path $labDir 'build'
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

$compiler = 'D:\GCC\mingw64\bin\g++.exe'
$dependencyRoot = 'D:\GCC\mingw64\opt'
if (-not (Test-Path -LiteralPath $compiler)) {
    throw 'MinGW g++ not found. Update the compiler path in build.ps1.'
}
if (-not (Test-Path -LiteralPath (Join-Path $dependencyRoot 'include\sqlite3.h'))) {
    throw 'SQLite headers not found. Update dependencyRoot in build.ps1.'
}

$flags = @(
    '-std=c++17', '-O2', '-Wall', '-Wextra', '-Wpedantic',
    ('-I' + (Join-Path $dependencyRoot 'include'))
)
$sources = @('db.cpp', 'util.cpp', 'account.cpp', 'event.cpp',
             'booking.cpp', 'governance.cpp') |
           ForEach-Object { Join-Path $sourceDir $_ }
$libraries = @(
    ('-L' + (Join-Path $dependencyRoot 'lib')),
    '-lsqlite3', '-lcrypto'
)

& $compiler @flags @sources (Join-Path $sourceDir 'main.cpp') @libraries `
    '-o' (Join-Path $buildDir 'ticket_app.exe')
if ($LASTEXITCODE -ne 0) { throw 'Application build failed.' }
& $compiler @flags @sources (Join-Path $sourceDir 'tests.cpp') @libraries `
    '-o' (Join-Path $buildDir 'ticket_tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Test build failed.' }

Write-Host "Build completed: $buildDir"
if ($RunTests) {
    $env:PATH = (Join-Path $dependencyRoot 'bin') + ';' +
                'D:\GCC\mingw64\bin;' + $env:PATH
    Push-Location $buildDir
    try {
        & (Join-Path $buildDir 'ticket_tests.exe')
        if ($LASTEXITCODE -ne 0) { throw 'Automated tests failed.' }
    } finally {
        Pop-Location
    }
}
