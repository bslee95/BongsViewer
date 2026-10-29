// BongsViewer.cpp  --  Win32 C++ Comic Book Viewer (ZIP/CBZ)
// Build: see build.bat
// Requires: miniz.h / miniz.c

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <ole2.h>
#include <commdlg.h>
#include <shellapi.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>

// miniz ZIP library (compiled separately)
#include "miniz.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;

// -------------------------------------------------------
//  Constants
// -------------------------------------------------------
static const wchar_t* APP_NAME  = L"BongsViewer";
static const wchar_t* WND_CLASS = L"BongsViewerWnd";

static const int TOOLBAR_H = 44;
static const int BTN_W     = 76;
static const int BTN_H     = 28;
static const int TRACK_W   = 220;

static const int ID_BTN_OPEN   = 101;
static const int ID_BTN_PREV   = 102;
static const int ID_BTN_NEXT   = 103;
static const int ID_TRACKBAR   = 104;
static const int ID_LABEL_PAGE = 105;

// -------------------------------------------------------
//  Image extension check
// -------------------------------------------------------
static bool IsImageFile(const char* name)
{
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    char low[16] = {};
    for (int i = 0; i < 15 && dot[i]; ++i)
        low[i] = (char)tolower((unsigned char)dot[i]);
    return strcmp(low, ".jpg")  == 0 ||
           strcmp(low, ".jpeg") == 0 ||
           strcmp(low, ".png")  == 0 ||
           strcmp(low, ".gif")  == 0 ||
           strcmp(low, ".bmp")  == 0 ||
           strcmp(low, ".webp") == 0;
}

// -------------------------------------------------------
//  ZipBook
// -------------------------------------------------------
struct PageEntry {
    std::string  filename;
    mz_uint      zipIndex;
};

class ZipBook {
public:
    mz_zip_archive zip = {};
    std::vector<PageEntry> pages;
    std::wstring filepath;
    bool isOpen = false;

    bool Open(const std::wstring& path)
    {
        Close();

        // Wide -> UTF-8
        int need = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1,
                                       NULL, 0, NULL, NULL);
        std::vector<char> utf8buf(need);
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1,
                            &utf8buf[0], need, NULL, NULL);

        memset(&zip, 0, sizeof(zip));
        if (!mz_zip_reader_init_file(&zip, &utf8buf[0], 0))
            return false;

        filepath = path;
        isOpen   = true;

        mz_uint count = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < count; ++i)
        {
            char namebuf[512] = {};
            mz_zip_reader_get_filename(&zip, i, namebuf, sizeof(namebuf));
            if (IsImageFile(namebuf))
                pages.push_back({ namebuf, i });
        }

        std::sort(pages.begin(), pages.end(),
            [](const PageEntry& a, const PageEntry& b){ return a.filename < b.filename; });

        return !pages.empty();
    }

    void Close()
    {
        if (isOpen) { mz_zip_reader_end(&zip); isOpen = false; }
        pages.clear();
        filepath.clear();
    }

    // Load page into heap buffer. Caller calls mz_free().
    void* LoadPage(int idx, size_t& outSize)
    {
        if (!isOpen || idx < 0 || idx >= (int)pages.size()) return NULL;
        return mz_zip_reader_extract_to_heap(&zip, pages[idx].zipIndex, &outSize, 0);
    }

    ~ZipBook() { Close(); }
};

// -------------------------------------------------------
//  App state
// -------------------------------------------------------
struct AppState {
    HWND hwndMain    = NULL;
    HWND hwndCanvas  = NULL;
    HWND hwndOpen    = NULL;
    HWND hwndPrev    = NULL;
    HWND hwndNext    = NULL;
    HWND hwndTrack   = NULL;
    HWND hwndPageLbl = NULL;

    ULONG_PTR gdipToken = 0;

    ZipBook book;
    int     currentPage = -1;
    Image*  image       = NULL;

    bool isFullscreen     = false;
    RECT prevWindowRect   = {};
    DWORD prevStyle       = 0;
    bool trackbarUpdating = false;

    float zoomFactor = 1.0f;  // 1.0 = fit-to-window, >1 = zoomed in, <1 = zoomed out

    std::wstring lastFile;
    std::wstring lastFolder;
    int          lastPage = 0;
};

static AppState g_app;

// -------------------------------------------------------
//  Forward declarations
// -------------------------------------------------------
LRESULT CALLBACK WndProc   (HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK CanvasProc(HWND, UINT, WPARAM, LPARAM);
void OpenZipDialog ();
void OpenZipFile   (const std::wstring& path, int startPage = 0, bool showError = true);
void GoToPage      (int idx);
void Navigate      (int delta);
void LayoutControls(HWND hwnd);
void ToggleFullscreen();
void PaintCanvas   (HWND hwnd);
void AdjustZoom    (float delta);
void SaveSettings  ();
void LoadSettings  ();

// -------------------------------------------------------
//  Settings (Last opened file, page, folder)
// -------------------------------------------------------
static std::string WideToUtf8(const std::wstring& wstr)
{
    if (wstr.empty()) return "";
    int need = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
    if (need <= 0) return "";
    std::string str(need - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], need, NULL, NULL);
    return str;
}

static std::wstring Utf8ToWide(const std::string& str)
{
    if (str.empty()) return L"";
    int need = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
    if (need <= 0) return L"";
    std::wstring wstr(need - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], need);
    return wstr;
}

static std::wstring GetSettingsPath()
{
    wchar_t appData[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData)))
    {
        std::wstring dir = std::wstring(appData) + L"\\BongsViewer";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\settings.ini";
    }
    return L"settings.ini";
}

void SaveSettings()
{
    std::wstring iniPath = GetSettingsPath();
    std::string content;
    if (!g_app.lastFolder.empty())
        content += "last_folder=" + WideToUtf8(g_app.lastFolder) + "\n";
    if (!g_app.lastFile.empty())
        content += "last_file=" + WideToUtf8(g_app.lastFile) + "\n";
    content += "last_page=" + std::to_string(g_app.lastPage) + "\n";

    FILE* f = NULL;
    if (_wfopen_s(&f, iniPath.c_str(), L"wb") == 0 && f)
    {
        fwrite(content.data(), 1, content.size(), f);
        fclose(f);
    }
}

void LoadSettings()
{
    std::wstring iniPath = GetSettingsPath();
    FILE* f = NULL;
    if (_wfopen_s(&f, iniPath.c_str(), L"rb") == 0 && f)
    {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 100000)
        {
            std::string buf(sz, '\0');
            fread(&buf[0], 1, sz, f);
            fclose(f);

            size_t start = 0;
            while (start < buf.size())
            {
                size_t end = buf.find('\n', start);
                if (end == std::string::npos) end = buf.size();
                std::string line = buf.substr(start, end - start);
                if (!line.empty() && line.back() == '\r') line.pop_back();

                size_t eq = line.find('=');
                if (eq != std::string::npos)
                {
                    std::string key = line.substr(0, eq);
                    std::string val = line.substr(eq + 1);
                    if (key == "last_file")        g_app.lastFile = Utf8ToWide(val);
                    else if (key == "last_folder") g_app.lastFolder = Utf8ToWide(val);
                    else if (key == "last_page")   g_app.lastPage = std::max(0, std::atoi(val.c_str()));
                }
                start = end + 1;
            }
            return;
        }
        fclose(f);
    }
}

// -------------------------------------------------------
//  WinMain
// -------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int nCmdShow)
{
    // GDI+
    GdiplusStartupInput gdipIn;
    GdiplusStartup(&g_app.gdipToken, &gdipIn, NULL);

    // Canvas window class
    WNDCLASSEXW cwc = {};
    cwc.cbSize        = sizeof(cwc);
    cwc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    cwc.lpfnWndProc   = CanvasProc;
    cwc.hInstance     = hInst;
    cwc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
    cwc.lpszClassName = L"BongsCanvas";
    RegisterClassExW(&cwc);

    // Main window class
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIcon(hInst, MAKEINTRESOURCE(101));
    wc.hIconSm       = (HICON)LoadImage(hInst, MAKEINTRESOURCE(101), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(20, 20, 20));
    wc.lpszClassName = WND_CLASS;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        WND_CLASS, APP_NAME,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 750,
        NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    LoadSettings();

    // If launched with a file argument, open it
    bool openedFromArg = false;
    if (lpCmdLine && lpCmdLine[0])
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argc > 1)
        {
            OpenZipFile(argv[1], 0, true);
            openedFromArg = true;
        }
        LocalFree(argv);
    }

    // Otherwise, resume last opened file and page if available
    if (!openedFromArg && !g_app.lastFile.empty())
    {
        DWORD attr = GetFileAttributesW(g_app.lastFile.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            OpenZipFile(g_app.lastFile, g_app.lastPage, false);
        }
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        // Intercept navigation/fullscreen keys at the app level so they work
        // regardless of which child control currently holds keyboard focus.
        if (msg.message == WM_KEYDOWN)
        {
            WPARAM vk = msg.wParam;
            if (vk == VK_LEFT  || vk == VK_RIGHT ||
                vk == VK_UP    || vk == VK_DOWN  ||
                vk == VK_PRIOR || vk == VK_NEXT  ||
                vk == VK_HOME  || vk == VK_END   ||
                vk == VK_SPACE || vk == VK_BACK  ||
                vk == VK_F11   || vk == VK_ESCAPE||
                vk == VK_RETURN||
                vk == 'F'      || vk == 'O'      ||
                vk == VK_OEM_PLUS || vk == VK_OEM_MINUS ||
                vk == VK_ADD      || vk == VK_SUBTRACT)
            {
                SendMessage(g_app.hwndMain, WM_KEYDOWN, msg.wParam, msg.lParam);
                continue;   // skip dispatching to the focused child control
            }
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(g_app.gdipToken);
    return (int)msg.wParam;
}

// -------------------------------------------------------
//  Create toolbar controls
// -------------------------------------------------------
static void CreateToolbarControls(HWND hwnd)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE);
    DWORD btnStyle  = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT;

    g_app.hwndOpen = CreateWindowW(L"BUTTON", L"[ Open ]",
        btnStyle, 8, 8, BTN_W, BTN_H,
        hwnd, (HMENU)(INT_PTR)ID_BTN_OPEN, hInst, NULL);

    g_app.hwndPrev = CreateWindowW(L"BUTTON", L"< Prev",
        btnStyle, 92, 8, BTN_W, BTN_H,
        hwnd, (HMENU)(INT_PTR)ID_BTN_PREV, hInst, NULL);

    g_app.hwndPageLbl = CreateWindowW(L"STATIC", L"0 / 0",
        WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
        176, 8, 90, BTN_H,
        hwnd, (HMENU)(INT_PTR)ID_LABEL_PAGE, hInst, NULL);

    g_app.hwndNext = CreateWindowW(L"BUTTON", L"Next >",
        btnStyle, 274, 8, BTN_W, BTN_H,
        hwnd, (HMENU)(INT_PTR)ID_BTN_NEXT, hInst, NULL);

    g_app.hwndTrack = CreateWindowW(TRACKBAR_CLASS, NULL,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS | TBS_TOOLTIPS,
        360, 10, TRACK_W, BTN_H - 4,
        hwnd, (HMENU)(INT_PTR)ID_TRACKBAR, hInst, NULL);

    EnableWindow(g_app.hwndPrev,  FALSE);
    EnableWindow(g_app.hwndNext,  FALSE);
    EnableWindow(g_app.hwndTrack, FALSE);

    HFONT hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessage(g_app.hwndOpen,    WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_app.hwndPrev,    WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_app.hwndNext,    WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessage(g_app.hwndPageLbl, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Canvas child window
    g_app.hwndCanvas = CreateWindowExW(0, L"BongsCanvas", NULL,
        WS_CHILD | WS_VISIBLE,
        0, TOOLBAR_H, 1000, 750 - TOOLBAR_H,
        hwnd, NULL,
        (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), NULL);

    DragAcceptFiles(hwnd, TRUE);
}

// -------------------------------------------------------
//  Layout controls
// -------------------------------------------------------
void LayoutControls(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    if (g_app.isFullscreen)
    {
        if (g_app.hwndCanvas)
            SetWindowPos(g_app.hwndCanvas, NULL, 0, 0, W, H, SWP_NOZORDER);
        return;
    }

    int trackX = 360;
    int trackW = W - trackX - 8;
    if (trackW < 40) trackW = 40;

    if (g_app.hwndTrack)
        SetWindowPos(g_app.hwndTrack, NULL, trackX, 10, trackW, BTN_H - 4, SWP_NOZORDER);

    if (g_app.hwndCanvas)
        SetWindowPos(g_app.hwndCanvas, NULL, 0, TOOLBAR_H, W, H - TOOLBAR_H, SWP_NOZORDER);
}

// -------------------------------------------------------
//  Open dialog
// -------------------------------------------------------
void OpenZipDialog()
{
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_app.hwndMain;
    ofn.lpstrFilter = L"Comic Book (*.zip;*.cbz)\0*.zip;*.cbz\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    if (!g_app.lastFolder.empty())
        ofn.lpstrInitialDir = g_app.lastFolder.c_str();
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle  = L"Open Comic Book ZIP";
    if (GetOpenFileNameW(&ofn))
        OpenZipFile(buf, 0, true);
}

// -------------------------------------------------------
//  Open ZIP file
// -------------------------------------------------------
void OpenZipFile(const std::wstring& path, int startPage, bool showError)
{
    if (g_app.image) { delete g_app.image; g_app.image = NULL; }
    g_app.currentPage = -1;

    if (!g_app.book.Open(path))
    {
        if (showError)
        {
            MessageBoxW(g_app.hwndMain,
                L"Cannot open ZIP or no images found.",
                L"Error", MB_OK | MB_ICONERROR);
        }
        return;
    }

    wchar_t dirBuf[MAX_PATH] = {};
    wcsncpy_s(dirBuf, path.c_str(), _TRUNCATE);
    PathRemoveFileSpecW(dirBuf);
    g_app.lastFolder = dirBuf;
    g_app.lastFile   = path;

    int total = (int)g_app.book.pages.size();

    SendMessage(g_app.hwndTrack, TBM_SETRANGEMIN, FALSE, 0);
    SendMessage(g_app.hwndTrack, TBM_SETRANGEMAX, FALSE, total - 1);
    SendMessage(g_app.hwndTrack, TBM_SETPOS, TRUE, 0);
    EnableWindow(g_app.hwndTrack, TRUE);
    EnableWindow(g_app.hwndPrev,  TRUE);
    EnableWindow(g_app.hwndNext,  TRUE);

    wchar_t title[512];
    swprintf_s(title, L"%s - %s", APP_NAME, PathFindFileNameW(path.c_str()));
    SetWindowTextW(g_app.hwndMain, title);

    if (startPage < 0) startPage = 0;
    if (startPage >= total) startPage = total - 1;

    GoToPage(startPage);
    SaveSettings();
}

// -------------------------------------------------------
//  Go to page
// -------------------------------------------------------
void GoToPage(int idx)
{
    int total = (int)g_app.book.pages.size();
    if (total == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= total) idx = total - 1;
    if (idx == g_app.currentPage) return;

    g_app.currentPage = idx;
    g_app.lastPage    = idx;
    g_app.zoomFactor  = 1.0f;  // reset zoom on page change
    SaveSettings();

    if (g_app.image) { delete g_app.image; g_app.image = NULL; }

    // Stream page data from ZIP into memory
    size_t dataSize = 0;
    void*  data     = g_app.book.LoadPage(idx, dataSize);
    if (data)
    {
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dataSize);
        if (hMem)
        {
            void* p = GlobalLock(hMem);
            memcpy(p, data, dataSize);
            GlobalUnlock(hMem);

            IStream* pStream = NULL;
            if (SUCCEEDED(CreateStreamOnHGlobal(hMem, TRUE, &pStream)))
            {
                g_app.image = Image::FromStream(pStream);
                pStream->Release();
            }
        }
        mz_free(data);
    }

    // Update trackbar
    g_app.trackbarUpdating = true;
    SendMessage(g_app.hwndTrack, TBM_SETPOS, TRUE, idx);
    g_app.trackbarUpdating = false;

    // Page label
    wchar_t lbl[64];
    swprintf_s(lbl, L"%d / %d", idx + 1, total);
    SetWindowTextW(g_app.hwndPageLbl, lbl);

    // Window title
    wchar_t title[512];
    swprintf_s(title, L"%s - %s  [%d/%d]",
        APP_NAME,
        PathFindFileNameW(g_app.book.filepath.c_str()),
        idx + 1, total);
    SetWindowTextW(g_app.hwndMain, title);

    InvalidateRect(g_app.hwndCanvas, NULL, TRUE);
}

void Navigate(int delta)
{
    GoToPage(g_app.currentPage + delta);
}

// -------------------------------------------------------
//  Zoom control  (+5% / -5%, centered on canvas)
// -------------------------------------------------------
void AdjustZoom(float delta)
{
    if (!g_app.image) return;
    g_app.zoomFactor += delta;
    if (g_app.zoomFactor < 0.05f) g_app.zoomFactor = 0.05f;  // minimum 5%
    if (g_app.zoomFactor > 10.0f) g_app.zoomFactor = 10.0f;  // maximum 1000%
    InvalidateRect(g_app.hwndCanvas, NULL, TRUE);
}

// -------------------------------------------------------
//  Toggle fullscreen
// -------------------------------------------------------
void ToggleFullscreen()
{
    HWND hwnd = g_app.hwndMain;
    if (!g_app.isFullscreen)
    {
        g_app.prevStyle = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &g_app.prevWindowRect);

        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMon, &mi);

        SetWindowLongPtr(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right  - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED);

        ShowWindow(g_app.hwndOpen,    SW_HIDE);
        ShowWindow(g_app.hwndPrev,    SW_HIDE);
        ShowWindow(g_app.hwndNext,    SW_HIDE);
        ShowWindow(g_app.hwndTrack,   SW_HIDE);
        ShowWindow(g_app.hwndPageLbl, SW_HIDE);

        g_app.isFullscreen = true;
    }
    else
    {
        SetWindowLongPtr(hwnd, GWL_STYLE, g_app.prevStyle);
        SetWindowPos(hwnd, NULL,
            g_app.prevWindowRect.left,
            g_app.prevWindowRect.top,
            g_app.prevWindowRect.right  - g_app.prevWindowRect.left,
            g_app.prevWindowRect.bottom - g_app.prevWindowRect.top,
            SWP_FRAMECHANGED | SWP_NOZORDER);

        ShowWindow(g_app.hwndOpen,    SW_SHOW);
        ShowWindow(g_app.hwndPrev,    SW_SHOW);
        ShowWindow(g_app.hwndNext,    SW_SHOW);
        ShowWindow(g_app.hwndTrack,   SW_SHOW);
        ShowWindow(g_app.hwndPageLbl, SW_SHOW);

        g_app.isFullscreen = false;
    }
    LayoutControls(hwnd);
    InvalidateRect(g_app.hwndCanvas, NULL, TRUE);
}

// -------------------------------------------------------
//  Paint canvas (double-buffered)
// -------------------------------------------------------
void PaintCanvas(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    // Off-screen bitmap
    HDC     hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbm    = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ hOld   = SelectObject(hdcMem, hbm);

    // Background fill
    HBRUSH bgBr = CreateSolidBrush(RGB(30, 30, 30));
    FillRect(hdcMem, &rc, bgBr);
    DeleteObject(bgBr);

    if (g_app.image && g_app.image->GetLastStatus() == Ok)
    {
        Graphics gfx(hdcMem);
        gfx.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        gfx.SetSmoothingMode(SmoothingModeHighQuality);
        gfx.SetPixelOffsetMode(PixelOffsetModeHighQuality);

        UINT imgW = g_app.image->GetWidth();
        UINT imgH = g_app.image->GetHeight();

        float scaleX = (float)W / (float)imgW;
        float scaleY = (float)H / (float)imgH;
        float scale  = scaleX < scaleY ? scaleX : scaleY;
        scale *= g_app.zoomFactor;

        int drawW = (int)(imgW * scale);
        int drawH = (int)(imgH * scale);
        int drawX = (W - drawW) / 2;
        int drawY = (H - drawH) / 2;

        gfx.DrawImage(g_app.image, drawX, drawY, drawW, drawH);
    }
    else if (g_app.book.pages.empty())
    {
        Graphics gfx(hdcMem);
        FontFamily ff(L"Segoe UI");
        Font font(&ff, 14, FontStyleRegular, UnitPixel);
        SolidBrush br(Color(255, 140, 140, 140));
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        RectF rctf(0.0f, 0.0f, (float)W, (float)H);
        const wchar_t* hint =
            L"Drag & drop a ZIP/CBZ file here, or click [ Open ]\n\n"
            L"Keys:  Left/Up = prev,  Right/Down = next,  PageUp/PageDown  Home/End\n"
            L"Mouse: Click left-half = prev,  right-half = next,  Wheel = scroll\n"
            L"F / F11 = fullscreen    Escape = exit fullscreen\n"
            L"+/- = zoom in/out (5% steps)";
        gfx.DrawString(hint, -1, &font, rctf, &sf, &br);
    }

    BitBlt(hdc, 0, 0, W, H, hdcMem, 0, 0, SRCCOPY);
    SelectObject(hdcMem, hOld);
    DeleteObject(hbm);
    DeleteDC(hdcMem);

    EndPaint(hwnd, &ps);
}

// -------------------------------------------------------
//  Canvas Window Procedure
// -------------------------------------------------------
LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
        PaintCanvas(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN:
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        Navigate(GET_X_LPARAM(lParam) > rc.right / 2 ? 1 : -1);
        return 0;
    }

    case WM_RBUTTONDOWN:
        Navigate(-1);
        return 0;

    case WM_MOUSEWHEEL:
        Navigate(GET_WHEEL_DELTA_WPARAM(wParam) < 0 ? 1 : -1);
        return 0;

    case WM_DROPFILES:
    {
        HDROP hDrop = (HDROP)wParam;
        wchar_t buf[MAX_PATH] = {};
        if (DragQueryFileW(hDrop, 0, buf, MAX_PATH))
            OpenZipFile(buf);
        DragFinish(hDrop);
        return 0;
    }

    case WM_KEYDOWN:
        SendMessage(g_app.hwndMain, WM_KEYDOWN, wParam, lParam);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// -------------------------------------------------------
//  Main Window Procedure
// -------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        g_app.hwndMain = hwnd;
        InitCommonControls();
        CreateToolbarControls(hwnd);
        return 0;

    case WM_SIZE:
        LayoutControls(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case ID_BTN_OPEN: OpenZipDialog(); break;
        case ID_BTN_PREV: Navigate(-1);    break;
        case ID_BTN_NEXT: Navigate(1);     break;
        }
        return 0;

    case WM_HSCROLL:
        if ((HWND)lParam == g_app.hwndTrack && !g_app.trackbarUpdating)
        {
            int pos = (int)SendMessage(g_app.hwndTrack, TBM_GETPOS, 0, 0);
            GoToPage(pos);
        }
        return 0;

    case WM_KEYDOWN:
        switch (wParam)
        {
        case VK_RIGHT: case VK_DOWN: case VK_NEXT:  case VK_SPACE: Navigate(1);  break;
        case VK_LEFT:  case VK_UP:   case VK_PRIOR: case VK_BACK:  Navigate(-1); break;
        case VK_HOME: GoToPage(0);                              break;
        case VK_END:  GoToPage((int)g_app.book.pages.size()-1); break;
        case VK_F11:  ToggleFullscreen();                       break;
        case 'F':     ToggleFullscreen();                       break;
        case VK_RETURN: ToggleFullscreen();                     break;
        case VK_ESCAPE:
            if (g_app.isFullscreen) ToggleFullscreen();         break;
        case 'O':
            if (GetKeyState(VK_CONTROL) & 0x8000) OpenZipDialog();
            break;
        // Zoom: +/= or numpad+ to zoom in, -/numpad- to zoom out
        case VK_OEM_PLUS: case VK_ADD:      AdjustZoom(+0.05f); break;
        case VK_OEM_MINUS: case VK_SUBTRACT: AdjustZoom(-0.05f); break;
        }
        return 0;

    case WM_MOUSEWHEEL:
        Navigate(GET_WHEEL_DELTA_WPARAM(wParam) < 0 ? 1 : -1);
        return 0;

    case WM_DROPFILES:
    {
        HDROP hDrop = (HDROP)wParam;
        wchar_t buf[MAX_PATH] = {};
        if (DragQueryFileW(hDrop, 0, buf, MAX_PATH))
            OpenZipFile(buf);
        DragFinish(hDrop);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC hdcCtl = (HDC)wParam;
        SetTextColor(hdcCtl, RGB(200, 200, 200));
        SetBkColor(hdcCtl, RGB(20, 20, 20));
        static HBRUSH sDarkBr = CreateSolidBrush(RGB(20, 20, 20));
        return (LRESULT)sDarkBr;
    }

    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        rc.bottom = TOOLBAR_H;
        HBRUSH br = CreateSolidBrush(RGB(20, 20, 20));
        FillRect(hdc, &rc, br);
        DeleteObject(br);
        return 1;
    }

    case WM_DESTROY:
        SaveSettings();
        if (g_app.image) { delete g_app.image; g_app.image = NULL; }
        g_app.book.Close();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
