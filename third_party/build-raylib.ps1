# raylib 6.0 정적 라이브러리 빌드 스크립트
#
# 왜 직접 빌드하는가:
#   vcpkg의 raylib 6.0 포트는 portfile.cmake에서 -DCUSTOMIZE_BUILD=ON 을 넘긴다.
#   그러면 raylib의 ParseConfigHeader가 src/config.h 의 항목들로 CMake 옵션을
#   자동 생성하는데, 이 과정에서 SUPPORT_CUSTOM_FRAME_CONTROL 이 기본 ON 으로
#   잡힌다(config.h 원본 값은 0). 그 결과 EndDrawing() 이 SwapScreenBuffer() 와
#   PollInputEvents() 를 호출하지 않아 창이 흰 화면으로 멈춘다.
#
#   CUSTOMIZE_BUILD=OFF(기본값)로 빌드하면 config.h 가 그대로 사용되어 문제가 없다.
#   덤으로 정적 링크라 raylib.dll / glfw3.dll 없이 exe 하나로 배포된다
#   (공모전 조건: 독립 실행파일 1.44MB 이하).
#
# 사용법:  pwsh -File third_party\build-raylib.ps1
# 산출물:  third_party\raylib\{include,lib}

$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $here "raylib-6.0"
$bld  = Join-Path $here "raylib-build"
$out  = Join-Path $here "raylib"

if (-not (Test-Path $src)) {
    throw "raylib 소스가 없습니다: $src`n" +
          "https://github.com/raysan5/raylib/archive/refs/tags/6.0.tar.gz 를 받아 " +
          "third_party\raylib-6.0 으로 풀어주세요."
}

cmake -S $src -B $bld -G "Visual Studio 17 2022" -A x64 `
    -DBUILD_SHARED_LIBS=OFF `
    -DBUILD_EXAMPLES=OFF `
    -DCUSTOMIZE_BUILD=OFF `
    -DUSE_EXTERNAL_GLFW=OFF `
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
    -DCMAKE_INSTALL_PREFIX="$out"

cmake --build $bld --config Release --target install

Write-Host "`n완료: $out" -ForegroundColor Green
