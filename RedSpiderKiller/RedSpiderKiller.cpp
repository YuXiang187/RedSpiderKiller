#include "stdafx.h"
#include "RedSpiderKiller.h"

#include "Killer.h"
#include "KbdBridge.h"
#include "Launcher.h"
#include "SecureDesktop.h"

#define MAX_LOADSTRING 100

// 工具栏图标尺寸与间距
const int ICON_SIZE = 32;
const int ICON_GAP = 10;

// 捕获到广播窗口之后，复查学生端窗口状态的间隔（毫秒）
const DWORD REFRESH_INTERVAL = 300;

// 工具栏按钮的动作
enum class ToolAction
{
	OpenChrome,
	OpenGameViewer,
	KeyboardBridge,
	SecureDesktop,
	ToggleAgent,
	Exit,
};

struct ToolbarButton
{
	ToolAction action;
	// 固定图标；ToggleAgent 的图标随显示状态变化，绘制时再取
	HICON icon;
	// 外部程序的完整路径，仅 Open* 动作使用
	std::wstring path;
};

// 只放真正可用的按钮：找不到的程序不占位置
std::vector<ToolbarButton> toolbarButtons;

// 工具栏宽度，始终只有一列
int ToolbarWidth()
{
	return ICON_GAP + (ICON_SIZE + ICON_GAP) * 1;
}

// 工具栏高度，随按钮个数变化
int ToolbarHeight()
{
	return ICON_GAP + (ICON_SIZE + ICON_GAP) * static_cast<int>(toolbarButtons.size());
}

// 第 index 个图标的顶部坐标
int IconTop(int index)
{
	return ICON_GAP + (ICON_SIZE + ICON_GAP) * index;
}

// 点击位置是否落在某个图标上
bool HitIcon(int x, int y, int top)
{
	return x > ICON_GAP && y > top && x < ICON_GAP + ICON_SIZE && y < top + ICON_SIZE;
}

// 按实际可用的程序组装工具栏
void BuildToolbar(HINSTANCE hInstance)
{
	toolbarButtons.clear();

	auto chromePath = Launcher::FindChrome();
	if (!chromePath.empty())
	{
		toolbarButtons.push_back({ ToolAction::OpenChrome, Launcher::LoadIcon32(chromePath), chromePath });
	}

	auto gameViewerPath = Launcher::FindGameViewer();
	if (!gameViewerPath.empty())
	{
		toolbarButtons.push_back({ ToolAction::OpenGameViewer, Launcher::LoadIcon32(gameViewerPath), gameViewerPath });
	}

	toolbarButtons.push_back({ ToolAction::KeyboardBridge,
		LoadIcon(hInstance, MAKEINTRESOURCE(IDI_KEY)), std::wstring() });

	toolbarButtons.push_back({ ToolAction::SecureDesktop,
		LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SECURE)), std::wstring() });

	toolbarButtons.push_back({ ToolAction::ToggleAgent, nullptr, std::wstring() });
	toolbarButtons.push_back({ ToolAction::Exit,
		LoadIcon(hInstance, MAKEINTRESOURCE(IDI_EXIT)), std::wstring() });
}

std::shared_ptr<Killer> killer = nullptr;

HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];

ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);

// 创建包含窗口用的函数
WCHAR szWindowClassTopWindow[] = L"TopWindow";
ATOM				MyRegisterClassTopWindow(HINSTANCE hInstance);
LRESULT CALLBACK	WndProcTopWindow(HWND, UINT, WPARAM, LPARAM);

int screenWidth;
int screenHeight;

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	// 被自己以特殊身份复用时，不创建任何窗口，做完事情直接退出
	if (SecureDesktop::HasSwitch(L"--send-sas"))
	{
		return SecureDesktop::RunSystemInstance();
	}

	if (SecureDesktop::HasSwitch(L"--secure-desktop"))
	{
		return SecureDesktop::RunElevatedInstance();
	}

	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);

	LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
	LoadStringW(hInstance, IDC_REDSPIDERKILLER, szWindowClass, MAX_LOADSTRING);
	MyRegisterClass(hInstance);
	MyRegisterClassTopWindow(hInstance);

	if (!InitInstance(hInstance, nCmdShow))
	{
		return FALSE;
	}

	HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_REDSPIDERKILLER));

	MSG msg;

	while (GetMessage(&msg, nullptr, 0, 0))
	{
		if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	return (int)msg.wParam;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
	WNDCLASSEXW wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);

	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = hInstance;
	wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_REDSPIDERKILLER));
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = nullptr;
	wcex.lpszClassName = szWindowClass;
	wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

	return RegisterClassExW(&wcex);
}

// 注册包含窗口类
ATOM MyRegisterClassTopWindow(HINSTANCE hInstance)
{
	WNDCLASSEXW wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);

	// 不重画、不擦背景：包含窗口整块被学生端窗口盖住，一旦它自己擦一次背景，
	// 露出来的就是一整屏底色，看着就是闪一下
	wcex.style = 0;
	wcex.lpfnWndProc = WndProcTopWindow;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = hInstance;
	wcex.hIcon = nullptr;
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = nullptr;
	wcex.lpszMenuName = nullptr;
	wcex.lpszClassName = szWindowClassTopWindow;
	wcex.hIconSm = nullptr;

	return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
	killer = Killer::Create();

	hInst = hInstance;

	// 先确定有哪些按钮，再按实际数量决定工具栏高度
	BuildToolbar(hInstance);

	screenWidth = GetSystemMetrics(SM_CXSCREEN);
	screenHeight = GetSystemMetrics(SM_CYSCREEN);

	HWND hWnd = CreateWindowEx(WS_EX_TOOLWINDOW, szWindowClass, szTitle, WS_POPUP,
		screenWidth - ToolbarWidth(), screenHeight / 3, ToolbarWidth(), ToolbarHeight(), nullptr, nullptr, hInstance, nullptr);

	if (!hWnd)
	{
		return FALSE;
	}

	// 创建包含窗口并设置，全屏无边框
	auto topWindow = CreateWindowEx(WS_EX_TOOLWINDOW, szWindowClassTopWindow, szTitle, WS_POPUP | WS_CLIPCHILDREN,
		0, 0, screenWidth, screenHeight, nullptr, nullptr, hInstance, nullptr);
	killer->SetTopWindow(topWindow);

	// 初始化
	if (!killer->Init())
	{
		auto mbresult = MessageBox(hWnd,
			L"未检测到已适配的学生端。\r\n\r\n"
			L"本程序支持：红蜘蛛 7.2（REDAgent.exe）、Veyon（veyon-worker.exe）、\r\n"
			L"极域课堂管理系统 V6（StudentMain.exe）。\r\n\r\n"
			L"请先启动其中一款，或者手动选择学生端程序。",
			szTitle, MB_OKCANCEL);
		if (mbresult == IDOK)
		{
			// 自动获取失败，手动填写
			static TCHAR szFileName[MAX_PATH];
			static TCHAR szTitleName[MAX_PATH];
			static TCHAR szFilter[] = TEXT("Execute Files(*.EXE)\0*.exe\0\0");

			OPENFILENAME ofn;
			ofn.lStructSize = sizeof(OPENFILENAME);
			ofn.hwndOwner = hWnd;
			ofn.hInstance = nullptr;
			ofn.lpstrFilter = szFilter;
			ofn.lpstrCustomFilter = nullptr;
			ofn.nMaxCustFilter = 0;
			ofn.nFilterIndex = 0;
			ofn.lpstrFile = szFileName;
			ofn.nMaxFile = MAX_PATH;
			ofn.lpstrFileTitle = szTitleName;
			ofn.nMaxFileTitle = MAX_PATH;
			ofn.lpstrInitialDir = nullptr;
			ofn.lpstrTitle = nullptr;
			ofn.Flags = 0;
			ofn.nFileOffset = 0;
			ofn.nFileExtension = 0;
			ofn.lpstrDefExt = TEXT("exe");
			ofn.lCustData = 0;
			ofn.lpfnHook = nullptr;
			ofn.lpTemplateName = nullptr;
			if (GetOpenFileName(&ofn))
			{
				killer->SetFilePath(std::wstring(szFileName));
			}
			else
			{
				return FALSE;
			}
		}
		else
		{
			return FALSE;
		}
	}

	// 键盘桥：先起线程备用，默认不装钩子（不做隐式全局读键）
	KbdBridge::Start();

	if (SecureDesktop::HasSwitch(L"--kbd-bridge"))
	{
		KbdBridge::SetEnabled(true);
	}

	// 只给自动化测试用：允许把注入的按键也当成物理键处理
	if (SecureDesktop::HasSwitch(L"--kbd-allow-injected"))
	{
		KbdBridge::SetAllowInjected(true);
	}

	// 排查用：强制退回投消息模式（中文会失效），确认是不是注入那条路的问题
	if (SecureDesktop::HasSwitch(L"--kbd-post"))
	{
		KbdBridge::SetForcePost(true);
	}

	ShowWindow(hWnd, nCmdShow);
	UpdateWindow(hWnd);

	return TRUE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	// 图标
	static HICON iconHide = LoadIcon(hInst, MAKEINTRESOURCE(IDI_HIDE));
	static HICON iconShow = LoadIcon(hInst, MAKEINTRESOURCE(IDI_SHOW));
	// 窗口的 X 坐标
	static int posX = screenWidth - ToolbarWidth();
	// 鼠标是否进入了窗口（不保证没有出去）
	static bool isMouseEntered = true;
	// 是否已经捕获到学生端的全屏广播窗口
	static bool agentWindowCaptured = false;
	// 查找广播窗口的轮询计时
	static DWORD lastProbe = 0;
	// 复查计时，已经捕获之后每隔一段时间再看一眼
	static DWORD lastRefresh = 0;
	// 上次设置工具栏位置时的 X 坐标
	static int lastPosX = 0x7FFFFFFF;
	// 上次补置顶的时间
	static DWORD lastAssert = 0;
	// 学生端窗口连续不可见的次数（用来区分"闪一下"和"真的结束了"）
	static int hiddenTicks = 0;
	// 计时，隐藏
	static int timeCount = 0;


	switch (message)
	{
	case WM_CREATE:
	{
		// 所有计时都用一个计时器
		SetTimer(hWnd, 0, 1, nullptr);
		break;
	}

	case WM_MOUSEMOVE:
	{
		isMouseEntered = true;
		timeCount = 0;
		break;
	}

	case WM_TIMER:
	{
		auto now = ::GetTickCount();

		// 查找并捕获学生端的全屏广播窗口（学生端未广播时抓不到）
		if (!agentWindowCaptured)
		{
			// 按 REFRESH_INTERVAL 轮询，不要跟着 1ms 计时器跑：
			// 每秒几十次 EnumWindows + OpenProcess 只会让窗口管理器空转
			if (now - lastProbe >= REFRESH_INTERVAL)
			{
				lastProbe = now;

				if (killer->AcquireAgentWindow())
				{
					killer->Window();
					killer->Show();
					agentWindowCaptured = true;
				}
			}
		}
		else
		{
			// 学生端可能重建窗口，或者把窗口从包含窗口里抢回桌面，隔一会儿复查一次。
			// 计时用 GetTickCount 而不是计时器次数：WM_TIMER 的实际精度只有十几毫秒
			if (now - lastRefresh >= REFRESH_INTERVAL)
			{
				lastRefresh = now;

				if (!killer->RefreshAgentWindows())
				{
					// 捕获到的窗口一个都不剩（学生端重启）
					killer->CloseAgentWindow();
					agentWindowCaptured = false;
					lastRefresh = 0;
					lastProbe = 0;
					hiddenTicks = 0;
				}
				else if (!killer->IsAgentWindowShowing())
				{
					// 学生端偶尔会把自己的窗口藏一下再显示（重画、切换全屏状态）。
					// 只藏一两个采样周期就当成广播结束的话，会不停地
					// 收起容器 -> 重新捕获，屏幕跟着闪。连续几次都不显示才算结束
					if (++hiddenTicks >= 3)
					{
						killer->CloseAgentWindow();
						agentWindowCaptured = false;
						lastRefresh = 0;
						lastProbe = 0;
						hiddenTicks = 0;
					}
				}
				else
				{
					hiddenTicks = 0;
				}
			}
		}
		// 工具栏显示与隐藏
		if (isMouseEntered && posX > screenWidth - ToolbarWidth())
		{
			posX -= 5;
		}
		if (!isMouseEntered && posX < screenWidth - 5)
		{
			posX += 5;
		}
		if (timeCount++ >= 250)
		{
			timeCount = 0;
			isMouseEntered = false;
		}

		// 位置没变就别重复 SetWindowPos。每秒几十次的置顶操作会让顶层窗口带
		// 反复重算，被包含的广播窗口跟着闪。只在需要时动，另外每秒补一次置顶
		if (posX != lastPosX || (killer->IsInited() && now - lastAssert >= 1000))
		{
			auto swp = SWP_NOSIZE | SWP_NOACTIVATE;
			if (!killer->IsInited())
			{
				swp |= SWP_NOZORDER;
			}

			SetWindowPos(hWnd, HWND_TOPMOST, posX, screenHeight / 3, 0, 0, swp);
			lastPosX = posX;
			lastAssert = now;
		}
		break;
	}

	case WM_LBUTTONUP:
	{
		auto x = LOWORD(lParam);
		auto y = HIWORD(lParam);

		for (size_t i = 0; i < toolbarButtons.size(); ++i)
		{
			if (!HitIcon(x, y, IconTop(static_cast<int>(i))))
			{
				continue;
			}

			const auto& button = toolbarButtons[i];
			switch (button.action)
			{
			case ToolAction::OpenChrome:
			case ToolAction::OpenGameViewer:
			{
				if (!Launcher::Open(button.path))
				{
					auto failureText = L"启动失败：\r\n" + button.path;
					MessageBox(hWnd, failureText.c_str(), L"打开失败", MB_OK);
				}
				break;
			}

			case ToolAction::KeyboardBridge:
			{
				KbdBridge::SetEnabled(!KbdBridge::IsEnabled());
				InvalidateRect(hWnd, nullptr, TRUE);
				break;
			}

			case ToolAction::SecureDesktop:
			{
				std::wstring error;
				if (!SecureDesktop::Trigger(error))
				{
					MessageBox(hWnd, error.c_str(), L"安全桌面", MB_OK | MB_ICONWARNING);
				}
				break;
			}

			case ToolAction::ToggleAgent:
			{
				if (agentWindowCaptured)
				{
					if (killer->IsHide())
					{
						killer->Show();
					}
					else
					{
						killer->Hide();
					}
					InvalidateRect(hWnd, nullptr, TRUE);
				}
				else
				{
					auto agents = killer->GetAgentNames();
					auto text = L"已识别到的学生端：" + (agents.empty() ? std::wstring(L"无") : agents) +
						L"\r\n\r\n全屏广播开始后会自动窗口化，\r\n窗口化后此按钮用于隐藏及显示，\r\n因此现在无效。";
					MessageBox(hWnd, text.c_str(), L"按钮不可用", MB_OK);
				}
				break;
			}

			case ToolAction::Exit:
			{
				auto mbresult = MessageBox(hWnd, L"关闭后红蜘蛛会恢复到原始状态，确定关闭吗？", L"警告", MB_YESNO);
				if (mbresult == IDYES)
				{
					SendMessage(hWnd, WM_DESTROY, 0, 0);
				}
				break;
			}
			}

			break;
		}

		break;
	}

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hWnd, &ps);

		// 只画真实存在的按钮，隐藏/显示按钮的图标随状态切换
		for (size_t i = 0; i < toolbarButtons.size(); ++i)
		{
			const auto& button = toolbarButtons[i];

			auto icon = button.icon;
			if (button.action == ToolAction::ToggleAgent)
			{
				icon = killer->IsHide() ? iconShow : iconHide;
			}

			if (icon == nullptr)
			{
				continue;
			}

			// 键盘桥是否开启，画成灰的/彩色的区分，一眼能看出状态
			if (button.action == ToolAction::KeyboardBridge && !KbdBridge::IsEnabled())
			{
				DrawState(hdc, nullptr, nullptr, reinterpret_cast<LPARAM>(icon), 0,
					ICON_GAP, IconTop(static_cast<int>(i)), ICON_SIZE, ICON_SIZE,
					DST_ICON | DSS_DISABLED);
				continue;
			}

			DrawIconEx(hdc, ICON_GAP, IconTop(static_cast<int>(i)), icon,
				ICON_SIZE, ICON_SIZE, 0, nullptr, DI_NORMAL);
		}

		EndPaint(hWnd, &ps);
		break;
	}

	case WM_DESTROY:
	{
		KillTimer(hWnd, 0);
		KbdBridge::Stop();
		PostQuitMessage(0);
		break;
	}

	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

// 包含窗口的窗口过程，啥都不干
LRESULT CALLBACK WndProcTopWindow(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_SETFOCUS:
		SetFocus(hWnd);
		break;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}