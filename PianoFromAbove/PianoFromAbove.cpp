/*************************************************************************************************
*
* File: PianoFromAbove.cpp
*
* Description: Main entry point for Piano From Above.
*              Creates windows and enters the GUI and game loops
*
* Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#include <Windows.h>
#include <CommCtrl.h>
#include <ctime>
#include <shlwapi.h>
#include <winhttp.h>
#include <regex>
#include <clocale>

#include "MainProcs.h"
#include "resource.h"

#include "Config.h"
#include "GameState.h"
#include "Renderer.h"
#include "Misc.h"

// Yes, I know you shouldn't store build numbers as doubles
constexpr double BUILD_VERSION = 20240112;

INT WINAPI WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpszCmdLine, INT nCmdShow );
DWORD WINAPI GameThread( LPVOID lpParameter );

//-----------------------------------------------------------------------------
// Global variables
//-----------------------------------------------------------------------------
HINSTANCE g_hInstance = NULL;
HWND g_hWnd = NULL;
HWND g_hWndBar = NULL;
HWND g_hWndGfx = NULL;
bool g_bGfxDestroyed = false;
TSQueue< MSG > g_MsgQueue; // Producer/consumer to hold events for our game thread

DWORD WINAPI UpdateCheckProc(LPVOID) {
    HINTERNET session = WinHttpOpen(L"pfavizkhang", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return 0;

    HINTERNET connect = WinHttpConnect(session, L"api.github.com",
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return 0;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", L"/repos/khang06/PianoFromAbove/releases",
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return 0;
    }

    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, NULL) &&
        WinHttpReceiveResponse(request, NULL))
    {
        std::string total;
        DWORD expected_len = 0;
        DWORD recv_len = 0;
        do {
            if (!WinHttpQueryDataAvailable(request, &expected_len))
                break;

            auto old_len = total.size();
            total.resize(old_len + expected_len);
            if (!WinHttpReadData(request, total.data() + old_len, expected_len, &recv_len))
                break;
        } while (expected_len != 0 && recv_len != 0);

        // Who needs a JSON library anyway
        std::regex regex("\"tag_name\":\\s*\"([0-9.]+)\",");
        std::smatch matches;
        
        if (std::regex_search(total, matches, regex)) {
            // C locale sucks so much
            auto old_locale = std::setlocale(LC_NUMERIC, nullptr);
            std::setlocale(LC_NUMERIC, "C");
            for (size_t i = 1; i < matches.size(); i++) {
                double parsed = 0.0;
                try {
                    parsed = std::stod(matches[i].str());
                } catch (...) {
                    continue;
                }
                if (parsed > BUILD_VERSION) {
                    auto menu = GetMenu(g_hWnd);
                    AppendMenu(menu, MF_STRING, ID_UPDATE, L"Update available!");
                    DrawMenuBar(g_hWnd);
                    break;
                }
            }
            std::setlocale(LC_NUMERIC, old_locale);
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    return 0;
}

//-----------------------------------------------------------------------------
// Name: wWinMain()
// Desc: The application's entry point
//-----------------------------------------------------------------------------
INT WINAPI WinMain( HINSTANCE hInstance, HINSTANCE, LPSTR, INT nCmdShow )
{
    g_hInstance = hInstance;
    srand( ( unsigned )time( NULL ) );

    // Ensure that the common control DLL is loaded. 
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof( INITCOMMONCONTROLSEX );
    icex.dwICC  = ICC_WIN95_CLASSES | ICC_COOL_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex); 

    // Initialize COM. For the SH* functions
    HRESULT hr = CoInitialize( NULL );
    if ( FAILED( hr ) ) return 1;

    // Register the window class
    WNDCLASSEX wc;
    wc.cbSize = sizeof( WNDCLASSEX );
    wc.style = 0;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0L;
    wc.cbWndExtra = 0L;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon( hInstance, MAKEINTRESOURCE( IDI_PFAICON ) );
    wc.hCursor = LoadCursor( NULL, IDC_ARROW );
    // Window is only a container... never seen, thus null brush
    wc.hbrBackground = NULL; //( HBRUSH )GetStockObject( NULL_BRUSH );
    wc.lpszMenuName = MAKEINTRESOURCE( IDM_MAINMENU );
    wc.lpszClassName = CLASSNAME;
    wc.hIconSm = NULL;
    if ( !RegisterClassEx( &wc ) )
        return 1;

    // Register the graphics window class
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = GfxProc;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = GFXCLASSNAME;
    if ( !RegisterClassEx( &wc ) )
        return 1;

    // Register the position control window class
    wc.style = 0;
    wc.lpfnWndProc = PosnProc;
    wc.lpszClassName = POSNCLASSNAME;
    if ( !RegisterClassEx( &wc ) )
        return 1;

    // In addition to getting settings, triggers loading of saved config
    Config &config = Config::GetConfig();
    ViewSettings &cView = config.GetViewSettings();
    PlaybackSettings &cPlayback = config.GetPlaybackSettings();

    // Bug the user if WinMM isn't patched
    if (!config.GetVizSettings().bKDMAPI || !LoadLibrary(L"OmniMIDI")) {
        wchar_t winmm_path[1024] = {};
        wchar_t win_dir[MAX_PATH] = {};
        if (GetModuleFileName(GetModuleHandle(L"winmm.dll"), winmm_path, _countof(winmm_path)) &&
            GetWindowsDirectory(win_dir, MAX_PATH) &&
            FindNLSString(0, LINGUISTIC_IGNORECASE, winmm_path, -1, win_dir, -1, NULL) == 0) {
            MessageBox(NULL, L"You don't appear to be using a patched winmm.dll.\nPlease patch it for best results.", L"", MB_ICONWARNING);
        }
    }

    // Create the application window
    g_hWnd = CreateWindowEx( 0, CLASSNAME, L"pfavizkhang-dx12 " __DATE__, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, cView.GetMainLeft(), cView.GetMainTop(),
                             cView.GetMainWidth(), cView.GetMainHeight(), NULL, NULL, wc.hInstance, NULL );
    if ( !g_hWnd ) return 1;

    // Accept drag and drop
    DragAcceptFiles(g_hWnd, true);

    // Creation order (z-order) matters big time for full screen

    // Create the controls rebar
    g_hWndBar = CreateRebar( g_hWnd );
    if ( !g_hWndBar ) return 1;

    // Create the graphics window
    g_hWndGfx = CreateWindowEx( 0, GFXCLASSNAME, NULL, WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS,
                                0, 0, 800, 600, g_hWnd, NULL, wc.hInstance, NULL );
    if ( !g_hWndGfx ) return 1;

    HACCEL hAccel = LoadAccelerators( hInstance, MAKEINTRESOURCE( IDA_MAINMENU ) );
    if ( !hAccel ) return 1;

    // Get the game going
    HANDLE hThread = CreateThread( NULL, 0, GameThread, new SplashScreen( NULL, NULL ), 0, NULL );
    if ( !hThread ) return 1;

    // Set up GUI and show
    SetPlayMode( GameState::Splash );
    SetOnTop( cView.GetOnTop() );
    ShowControls( cView.GetControls() );
    // Ensure the window is on-screen
    RECT rcMain;
    GetWindowRect( g_hWnd, &rcMain );
    if ( MonitorFromRect( &rcMain, MONITOR_DEFAULTTONULL ) == NULL )
        SetWindowPos( g_hWnd, NULL, 100, 100, 1024, 768, SWP_NOZORDER | SWP_NOACTIVATE );

    ShowWindow( g_hWndGfx, SW_SHOW );
    ShowWindow( g_hWnd, ( nCmdShow == SW_SHOWMINIMIZED || nCmdShow == SW_MINIMIZE ) ? SW_SHOWNORMAL : nCmdShow );
    UpdateWindow( g_hWnd );
    SetFocus( g_hWndGfx );
    cPlayback.SetPaused( false, false );

    // Spawn update check thread
    CreateThread(NULL, 0, UpdateCheckProc, NULL, 0, NULL);

    // Enter the message loop
    MSG msg = {};
    while( GetMessage( &msg, NULL, 0, 0 ) )
    {
        if( !TranslateAccelerator( g_hWnd, hAccel, &msg ) &&
            !IsDialogMessage( g_hWnd, &msg ) )
        {
            TranslateMessage( &msg );
            DispatchMessage( &msg );
        }
    }

    // Signal the game thread to exit and wait for it
    g_MsgQueue.ForcePush( msg );
    WaitForSingleObject( hThread, INFINITE );

    // Save settings
    config.SaveConfigValues();

    // Clean up
    UnregisterClass( CLASSNAME, wc.hInstance );
    CoUninitialize();
    return 0;
}

DWORD WINAPI GameThread( LPVOID lpParameter )
{
    if ( !g_hWndGfx ) return 0;

    // Initialize Direct3D
    D3D12Renderer *pRenderer = new D3D12Renderer();
    auto init_res = pRenderer->Init(g_hWndGfx, Config::GetConfig().GetVideoSettings().bLimitFPS);
    if( FAILED(std::get<0>(init_res)) )
    {
        wchar_t msg[1024] = {};
        _snwprintf_s(msg, 1024, L"Fatal error initializing D3D12.\n%S failed with code 0x%x.", std::get<1>(init_res), std::get<0>(init_res));
        MessageBox( g_hWnd, msg, TEXT( "Error" ), MB_OK | MB_ICONEXCLAMATION );
        PostMessage( g_hWnd, WM_QUIT, 1, 0 );
        return 1;
    }

    // Create the game object
    GameState *pGameState = reinterpret_cast< GameState* >( lpParameter );
    pGameState->SetHWnd( g_hWndGfx );
    pGameState->SetRenderer( pRenderer );
    pGameState->Init();
    GameState::GameError ge;

    // Put the adapter in the window title
    wchar_t buf[1024] = {};
#ifdef __AVX2__
    _snwprintf_s(buf, 1024, L"pfavizkhang-dx12 %S (AVX2 build, Device: %s)", __DATE__, pRenderer->GetAdapterName().c_str());
#else
    _snwprintf_s(buf, 1024, L"pfavizkhang-dx12 %S (SSE4.2 build, Device: %s)", __DATE__, pRenderer->GetAdapterName().c_str());
#endif
    SetWindowTextW(g_hWnd, buf);

    // Event, logic, render...
    MSG msg = {};
    while( msg.message != WM_QUIT )
    {
        while ( g_MsgQueue.Pop( msg ) )
            pGameState->MsgProc( msg.hwnd, msg.message, msg.wParam, msg.lParam );

        if ( ( ge = GameState::ChangeState( pGameState->NextState(), &pGameState ) ) != GameState::Success )
            PostMessage( g_hWnd, WM_COMMAND, ID_GAMEERROR, ge );
        pGameState->Logic();
        pGameState->Render();
    }

    delete pGameState;
    delete pRenderer;

    return 0;
}