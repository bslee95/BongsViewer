@echo off
setlocal

set "VSBASE=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build"
set "VCVARS=%VSBASE%\vcvars64.bat"

if not exist "%VCVARS%" (
    echo Visual Studio vcvars64.bat not found at:
    echo   %VCVARS%
    pause
    exit /b 1
)

echo Visual Studio 환경 설정 중...
call "%VCVARS%" >nul 2>&1

echo BongsViewer 빌드 중...

rc.exe /nologo BongsViewer.rc
if %ERRORLEVEL% neq 0 (
    echo 리소스 컴파일 실패
    pause
    exit /b 1
)

cl.exe /nologo /EHsc /O2 /W3 /MT ^
    /DUNICODE /D_UNICODE ^
    /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 ^
    /Fe:BongsViewer.exe ^
    BongsViewer.cpp miniz.c BongsViewer.res ^
    /link /SUBSYSTEM:WINDOWS ^
    gdiplus.lib comctl32.lib comdlg32.lib shell32.lib shlwapi.lib ^
    user32.lib gdi32.lib ole32.lib kernel32.lib

if %ERRORLEVEL% == 0 (
    echo.
    echo ==============================
    echo  빌드 성공! BongsViewer.exe
    echo ==============================
) else (
    echo.
    echo 빌드 실패
    pause
)

endlocal
