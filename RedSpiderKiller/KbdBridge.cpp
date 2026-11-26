#include "stdafx.h"
#include "KbdBridge.h"

#include <atomic>

namespace
{
	// 重装周期。对方占用链首期间的按键会丢，周期越短丢得越少；
	// 但每次重装都有一个极短的空窗，300ms 是实测下来的折中
	const DWORD REINSTALL_INTERVAL = 300;

	const UINT MSG_SET_ENABLED = WM_APP + 101;
	const UINT MSG_TICK = WM_APP + 102;

	// 桥自己注入的按键都带上这个标记。钩子里认出来就直接放行，
	// 不再重放一次，否则自己的注入会被自己的钩子再投一遍，无限循环
	const ULONG_PTR INJECT_TAG = 0x52534B42; // 'RSKB'

	std::atomic<bool> g_enabled{ false };
	std::atomic<bool> g_allowInjected{ false };
	std::atomic<bool> g_forcePost{ false };

	// 设置环境变量 KBD_BRIDGE_DEBUG 后，把过程写进 %TEMP%\kbdbridge.log（纯 ASCII，方便看）
	FILE* g_debugLog = nullptr;

	void DebugLog(const char* format, ...)
	{
		if (g_debugLog == nullptr)
		{
			return;
		}

		char line[512] = { 0 };

		va_list args;
		va_start(args, format);
		_vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
		va_end(args);

		fprintf(g_debugLog, "%s\n", line);
		fflush(g_debugLog);
	}

	// 重装间隔可被环境变量覆盖，方便排查
	DWORD ReinstallInterval()
	{
		static DWORD interval = 0;
		if (interval != 0)
		{
			return interval;
		}

		interval = REINSTALL_INTERVAL;

		char text[16] = { 0 };
		if (::GetEnvironmentVariableA("KBD_BRIDGE_INTERVAL", text, sizeof(text)) > 0)
		{
			int value = atoi(text);
			if (value >= 100 && value <= 60000)
			{
				interval = static_cast<DWORD>(value);
			}
		}

		return interval;
	}

	HHOOK g_hook = nullptr;
	HANDLE g_stopEvent = nullptr;
	DWORD g_threadId = 0;
	HANDLE g_thread = nullptr;
	HANDLE g_ticker = nullptr;
	HANDLE g_threadReady = nullptr;

	// 修饰键状态。物理键被对方的钩子吞掉以后 GetKeyState 是脏的，
	// 靠不住，只能自己按事件记账
	bool g_shift = false;
	bool g_ctrl = false;
	bool g_alt = false;
	bool g_caps = false;

	// 已经由桥投递、并且把 keydown 吞掉的键；对应的 keyup 也要吞，
	// 否则目标程序会收到一个没有按下过的抬起事件
	bool g_swallowed[256] = { false };

	bool IsKeyUp(WPARAM wparam)
	{
		return wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
	}

	bool IsExtended(DWORD vk)
	{
		switch (vk)
		{
		case VK_LEFT:
		case VK_RIGHT:
		case VK_UP:
		case VK_DOWN:
		case VK_HOME:
		case VK_END:
		case VK_PRIOR:
		case VK_NEXT:
		case VK_INSERT:
		case VK_DELETE:
		case VK_RCONTROL:
		case VK_RMENU:
		case VK_NUMLOCK:
		case VK_DIVIDE:
		case VK_LWIN:
		case VK_RWIN:
		case VK_APPS:
			return true;

		default:
			return false;
		}
	}

	void UpdateModifiers(DWORD vk, bool up)
	{
		switch (vk)
		{
		case VK_SHIFT:
		case VK_LSHIFT:
		case VK_RSHIFT:
			g_shift = !up;
			break;

		case VK_CONTROL:
		case VK_LCONTROL:
		case VK_RCONTROL:
			g_ctrl = !up;
			break;

		case VK_MENU:
		case VK_LMENU:
		case VK_RMENU:
			g_alt = !up;
			break;

		case VK_CAPITAL:
			if (!up)
			{
				g_caps = !g_caps;
			}
			break;

		default:
			break;
		}
	}

	// 用前台线程的键盘布局翻译，多语言环境下才对得上
	HKL ForegroundLayout()
	{
		HWND foreground = ::GetForegroundWindow();
		if (foreground != nullptr)
		{
			DWORD threadId = ::GetWindowThreadProcessId(foreground, nullptr);
			HKL layout = ::GetKeyboardLayout(threadId);
			if (layout != nullptr)
			{
				return layout;
			}
		}

		return ::GetKeyboardLayout(0);
	}

	// ------------------------------------------------------------------
	// 注入模式：把按键原样塞回系统输入队列
	//
	// 红蜘蛛的钩子只吞「物理」按键，对它自己的远控注入是放行的（否则远控就没法用了），
	// 所以真实按键注入能穿过它的锁。这条路走的是系统正常输入栈，输入法、快捷键、
	// 方向键全部和真键盘一样工作。
	// ------------------------------------------------------------------

	void InjectKey(WORD vk, DWORD scan, bool up, bool extended)
	{
		INPUT input = { 0 };
		input.type = INPUT_KEYBOARD;
		input.ki.wVk = vk;
		input.ki.wScan = static_cast<WORD>(scan);
		input.ki.dwFlags = static_cast<DWORD>(up ? KEYEVENTF_KEYUP : 0)
			| (extended ? KEYEVENTF_EXTENDEDKEY : 0);
		input.ki.dwExtraInfo = INJECT_TAG;

		::SendInput(1, &input, sizeof(INPUT));
	}

	// 钩子里绝对不能直接 SendInput：LL 钩子是系统同步调进来的，系统正等着钩子
	// 返回，这时再要求系统处理新输入，两边互等，最后钩子超时被系统丢掉。
	// 所以钩子只把事件塞进队列，由专门的注入线程在钩子外面补投。
	struct PendingKey
	{
		WORD vk;
		DWORD scan;
		bool up;
		bool extended;
	};

	const int PENDING_MAX = 256;

	CRITICAL_SECTION g_queueLock;
	bool g_queueReady = false;
	PendingKey g_pending[PENDING_MAX];
	int g_pendingCount = 0;
	HANDLE g_relayEvent = nullptr;
	HANDLE g_relayThread = nullptr;

	void QueueKey(WORD vk, DWORD scan, bool up, bool extended)
	{
		if (!g_queueReady)
		{
			return;
		}

		::EnterCriticalSection(&g_queueLock);

		// 队列满了就丢（极端连打才会发生），宁可丢键也不能卡住钩子
		if (g_pendingCount < PENDING_MAX)
		{
			PendingKey& slot = g_pending[g_pendingCount++];
			slot.vk = vk;
			slot.scan = scan;
			slot.up = up;
			slot.extended = extended;
		}

		::LeaveCriticalSection(&g_queueLock);
		::SetEvent(g_relayEvent);
	}

	void ClearQueue()
	{
		if (!g_queueReady)
		{
			return;
		}

		::EnterCriticalSection(&g_queueLock);
		g_pendingCount = 0;
		::LeaveCriticalSection(&g_queueLock);
	}

	DWORD WINAPI RelayWorker(LPVOID)
	{
		HANDLE waits[2] = { g_stopEvent, g_relayEvent };

		while (true)
		{
			DWORD wait = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);

			if (wait != WAIT_OBJECT_0 + 1)
			{
				break; // 收到停止信号
			}

			INPUT batch[PENDING_MAX] = {};
			int count = 0;

			::EnterCriticalSection(&g_queueLock);
			for (int i = 0; i < g_pendingCount; ++i)
			{
				const PendingKey& key = g_pending[i];

				INPUT& input = batch[count++];
				input.type = INPUT_KEYBOARD;
				input.ki.wVk = key.vk;
				input.ki.wScan = static_cast<WORD>(key.scan);
				input.ki.dwFlags = static_cast<DWORD>(key.up ? KEYEVENTF_KEYUP : 0)
					| (key.extended ? KEYEVENTF_EXTENDEDKEY : 0);
				input.ki.dwExtraInfo = INJECT_TAG;
			}
			g_pendingCount = 0;
			::LeaveCriticalSection(&g_queueLock);

			if (count > 0)
			{
				::SendInput(static_cast<UINT>(count), batch, sizeof(INPUT));
			}
		}

		return 0;
	}

	// 当前进程的完整性级别（SID 的最后一个子认证），提权与否就比这个
	bool GetIntegrityLevel(HANDLE process, DWORD& level)
	{
		HANDLE token = nullptr;
		if (!::OpenProcessToken(process, TOKEN_QUERY, &token))
		{
			return false;
		}

		BYTE buffer[256] = { 0 };
		DWORD size = 0;
		BOOL ok = ::GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size);
		::CloseHandle(token);

		if (!ok)
		{
			return false;
		}

		auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer);
		if (label->Label.Sid == nullptr)
		{
			return false;
		}

		BYTE* count = ::GetSidSubAuthorityCount(label->Label.Sid);
		if (count == nullptr || *count == 0)
		{
			return false;
		}

		DWORD* rid = ::GetSidSubAuthority(label->Label.Sid, static_cast<DWORD>(*count) - 1);
		if (rid == nullptr)
		{
			return false;
		}

		level = *rid;
		return true;
	}

	// 目标进程不比我们权限高才敢注入：UIPI 拦下来的注入是静默失败的，
	// 不报错也不生效，只能事先判掉，退回投消息
	bool CanInjectInto(DWORD processId)
	{
		DWORD selfLevel = 0;
		if (!GetIntegrityLevel(::GetCurrentProcess(), selfLevel))
		{
			return false;
		}

		HANDLE target = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
		if (target == nullptr)
		{
			return false;
		}

		DWORD targetLevel = 0;
		bool ok = GetIntegrityLevel(target, targetLevel);
		::CloseHandle(target);

		return ok && targetLevel <= selfLevel;
	}

	// 每个前台窗口只判一次：钩子回调里不能做重活，还要经常换窗口
	HWND g_checkedWindow = nullptr;
	bool g_checkedResult = false;

	bool RelayAllowed(DWORD targetProcessId)
	{
		if (g_forcePost.load() || targetProcessId == ::GetCurrentProcessId())
		{
			return false;
		}

		HWND foreground = ::GetForegroundWindow();
		if (foreground != nullptr && foreground == g_checkedWindow)
		{
			return g_checkedResult;
		}

		g_checkedWindow = foreground;
		g_checkedResult = CanInjectInto(targetProcessId);

		DebugLog("relay check hwnd=%p pid=%lu ok=%d", foreground, targetProcessId, g_checkedResult ? 1 : 0);

		return g_checkedResult;
	}

	// ------------------------------------------------------------------
	// 投递模式：往目标窗口的消息队列里塞消息
	//
	// 只在注入不可用时当兜底（目标是提权进程）。这条路绕过了输入法，
	// 中文打不出来，只能出 ASCII。
	// ------------------------------------------------------------------

	// 把消息投给前台窗口的焦点控件。注意这里不经过任何输入队列，
	// 是直接塞进目标窗口的消息队列，所以锁键盘的手段一个都拦不到
	void DeliverMessage(UINT message, WPARAM wparam, LPARAM lparam)
	{
		HWND foreground = ::GetForegroundWindow();
		if (foreground == nullptr)
		{
			return;
		}

		DWORD processId = 0;
		DWORD threadId = ::GetWindowThreadProcessId(foreground, &processId);

		// 别往自己的工具栏里打
		if (processId == ::GetCurrentProcessId())
		{
			return;
		}

		HWND target = foreground;

		GUITHREADINFO info = { sizeof(info) };
		if (::GetGUIThreadInfo(threadId, &info) && info.hwndFocus != nullptr)
		{
			target = info.hwndFocus;
		}

		DebugLog("deliver msg=0x%04X wparam=0x%04X to hwnd=%p",
			message, static_cast<UINT>(wparam), target);

		::PostMessageW(target, message, wparam, lparam);
	}

	void DeliverChar(wchar_t ch, DWORD scanCode)
	{
		auto lparam = 1 | (static_cast<LPARAM>(scanCode) << 16);

		DeliverMessage(WM_CHAR, static_cast<WPARAM>(ch), lparam);
	}

	// 编辑类按键（退格、方向键、Delete 等）必须走 WM_KEYDOWN：
	// 浏览器、Electron 一类程序只认按键消息，自己转字符，投 WM_CHAR 它们不理
	void DeliverKey(WORD vk, DWORD scanCode, bool extended)
	{
		auto lparam = 1 | (static_cast<LPARAM>(scanCode) << 16)
			| (extended ? (static_cast<LPARAM>(1) << 24) : 0);

		DeliverMessage(WM_KEYDOWN, vk, lparam);
		DeliverMessage(WM_KEYUP, vk, lparam | (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31));
	}

	// 返回 true 表示这个键已经由桥投递出去了，调用方要把它吞掉，
	// 否则原生路径还会再送一遍，变成双份字符
	bool HandleKeyDown(const KBDLLHOOKSTRUCT& info)
	{
		auto vk = static_cast<WORD>(info.vkCode);

		// 修饰键自己不进字符
		switch (vk)
		{
		case VK_SHIFT:
		case VK_LSHIFT:
		case VK_RSHIFT:
		case VK_CONTROL:
		case VK_LCONTROL:
		case VK_RCONTROL:
		case VK_MENU:
		case VK_LMENU:
		case VK_RMENU:
		case VK_CAPITAL:
		case VK_LWIN:
		case VK_RWIN:
			return false;

		default:
			break;
		}

		// Ctrl/Alt 组合是快捷键，目标程序查的是 GetKeyState，投消息没用
		if (g_ctrl || g_alt)
		{
			return false;
		}

		switch (vk)
		{
		case VK_RETURN:
		case VK_BACK:
		case VK_TAB:
		case VK_DELETE:
		case VK_INSERT:
		case VK_LEFT:
		case VK_RIGHT:
		case VK_UP:
		case VK_DOWN:
		case VK_HOME:
		case VK_END:
		case VK_PRIOR:
		case VK_NEXT:
			DeliverKey(vk, info.scanCode, IsExtended(vk));
			return true;

		case VK_ESCAPE:
			return false;

		default:
			break;
		}

		BYTE state[256] = { 0 };
		if (g_shift)
		{
			state[VK_SHIFT] = 0x80;
		}
		if (g_ctrl)
		{
			state[VK_CONTROL] = 0x80;
		}
		if (g_alt)
		{
			state[VK_MENU] = 0x80;
		}
		if (g_caps)
		{
			state[VK_CAPITAL] = 0x01;
		}

		wchar_t buffer[8] = { 0 };
		int count = ::ToUnicodeEx(vk, info.scanCode, state, buffer, _countof(buffer), 0, ForegroundLayout());

		// 死键（法语、德语布局里的重音键）返回负数，忽略即可
		if (count <= 0)
		{
			return false;
		}

		for (int i = 0; i < count; ++i)
		{
			DeliverChar(buffer[i], info.scanCode);
		}

		return true;
	}

	// 关桥的时候把还按着的修饰键补一个抬起，不然系统里会卡住一个 Shift/Ctrl
	void ReleaseModifiers()
	{
		if (g_shift)
		{
			InjectKey(VK_SHIFT, static_cast<DWORD>(::MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC)), true, false);
		}

		if (g_ctrl)
		{
			InjectKey(VK_CONTROL, static_cast<DWORD>(::MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC)), true, false);
		}

		if (g_alt)
		{
			InjectKey(VK_MENU, static_cast<DWORD>(::MapVirtualKeyW(VK_MENU, MAPVK_VK_TO_VSC)), true, false);
		}

		g_shift = false;
		g_ctrl = false;
		g_alt = false;
	}

	LRESULT CALLBACK HookProc(int code, WPARAM wparam, LPARAM lparam)
	{
		if (code == HC_ACTION && g_enabled)
		{
			const auto& info = *reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);

			bool injected = (info.flags & LLKHF_INJECTED) != 0;
			bool up = IsKeyUp(wparam);

			// 桥自己注入的按键：原样放行，绝不二次投递
			if (injected && info.dwExtraInfo == INJECT_TAG)
			{
				return ::CallNextHookEx(nullptr, code, wparam, lparam);
			}

			DebugLog("key vk=0x%02X scan=0x%02X up=%d injected=%d",
				info.vkCode, info.scanCode, up ? 1 : 0, injected ? 1 : 0);

			if (!injected || g_allowInjected.load())
			{
				DWORD processId = 0;
				HWND foreground = ::GetForegroundWindow();
				if (foreground != nullptr)
				{
					::GetWindowThreadProcessId(foreground, &processId);
				}

				if (RelayAllowed(processId))
				{
					// 原样中继：按下投按下，抬起投抬起，修饰键也照投，
					// 这样输入法的 Shift 切中英文、Ctrl+Space、快捷键、自动重复全都正常
					UpdateModifiers(info.vkCode, up);
					QueueKey(static_cast<WORD>(info.vkCode), info.scanCode, up, IsExtended(info.vkCode));

					// 吞掉物理键，避免它万一被放行时打两份
					return 1;
				}

				UpdateModifiers(info.vkCode, up);

				if (up)
				{
					// 按下时被吞掉的键，抬起也要吞，成对处理
					if (g_swallowed[info.vkCode & 0xFF])
					{
						g_swallowed[info.vkCode & 0xFF] = false;
						return 1;
					}
				}
				else if (HandleKeyDown(info))
				{
					// 已经由桥投递出去了，就别再让原生路径送一遍，
					// 否则没锁键盘的时候会打出一模一样的两份
					g_swallowed[info.vkCode & 0xFF] = true;
					return 1;
				}
			}
		}

		// 没投递的键（修饰键、Ctrl/Alt 组合、认不出的键）原样放行，
		// 快捷键和输入法在桥开着的时候照样能用
		return ::CallNextHookEx(nullptr, code, wparam, lparam);
	}

	void InstallHook()
	{
		// 先装新的、再卸旧的：反过来的话中间会有一个"没有我们的钩子"的空窗，
		// 正好被对方的钩子占住，那一下的按键就丢了（实测丢过字符）
		HHOOK fresh = ::SetWindowsHookExW(WH_KEYBOARD_LL, HookProc, ::GetModuleHandleW(nullptr), 0);

		DebugLog("install hook=%p err=%lu", fresh, fresh != nullptr ? 0UL : ::GetLastError());

		if (fresh == nullptr)
		{
			// 装不上就保留旧的，别把自己弄瞎
			return;
		}

		HHOOK stale = g_hook;
		g_hook = fresh;

		if (stale != nullptr)
		{
			::UnhookWindowsHookEx(stale);
		}
	}

	void ApplyEnabled(bool enabled)
	{
		DebugLog("apply enabled=%d", enabled ? 1 : 0);

		if (enabled)
		{
			g_enabled = true;
			InstallHook();
		}
		else
		{
			g_enabled = false;

			if (g_hook != nullptr)
			{
				::UnhookWindowsHookEx(g_hook);
				g_hook = nullptr;
			}

			// 队列里还没投出去的键丢掉，再把按着的修饰键松开
			ClearQueue();
			ReleaseModifiers();
			g_caps = false;
		}
	}

	DWORD WINAPI Worker(LPVOID)
	{
		g_threadId = ::GetCurrentThreadId();

		// 先把消息队列建出来，否则 PostThreadMessage 会失败
		MSG msg;
		::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

		if (g_threadReady != nullptr)
		{
			::SetEvent(g_threadReady);
		}

		// 必须用 GetMessage 阻塞等待：LL 钩子的回调只有在 GetMessage 的
		// 等待状态里才会被投递进来，换成 PeekMessage + MsgWaitForMultipleObjects
		// 之后钩子会静悄悄地一次都不触发（实测踩过）
		while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
		{
			if (msg.message == MSG_SET_ENABLED)
			{
				ApplyEnabled(msg.wParam != 0);
				continue;
			}

			if (msg.message == MSG_TICK)
			{
				// 对方在开始广播时会把它的钩子装到链首，这里定时重装抢回来
				if (g_enabled)
				{
					InstallHook();
				}
				continue;
			}

			::TranslateMessage(&msg);
			::DispatchMessageW(&msg);
		}

		if (g_hook != nullptr)
		{
			::UnhookWindowsHookEx(g_hook);
			g_hook = nullptr;
		}

		return 0;
	}

	// 独立的节拍线程：线程 SetTimer 在这种长阻塞的 GetMessage 里并不可靠
	// （实测一次都不触发），改成外部定时 PostThreadMessage 把 worker 叫醒
	DWORD WINAPI Ticker(LPVOID)
	{
		while (true)
		{
			DWORD wait = ::WaitForSingleObject(g_stopEvent, ReinstallInterval());

			if (wait != WAIT_TIMEOUT)
			{
				break;
			}

			if (g_enabled && g_threadId != 0)
			{
				::PostThreadMessageW(g_threadId, MSG_TICK, 0, 0);
			}
		}

		return 0;
	}
}

void KbdBridge::Start()
{
	if (g_thread != nullptr)
	{
		return;
	}

	wchar_t debugFlag[8] = { 0 };
	if (::GetEnvironmentVariableW(L"KBD_BRIDGE_DEBUG", debugFlag, _countof(debugFlag)) > 0)
	{
		char tempPath[MAX_PATH] = { 0 };
		::GetTempPathA(MAX_PATH, tempPath);
		std::string logPath = std::string(tempPath) + "kbdbridge.log";
		fopen_s(&g_debugLog, logPath.c_str(), "w");
	}

	g_threadReady = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	g_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

	::InitializeCriticalSection(&g_queueLock);
	g_queueReady = true;

	g_relayEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
	g_relayThread = ::CreateThread(nullptr, 0, RelayWorker, nullptr, 0, nullptr);

	g_thread = ::CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);

	if (g_threadReady != nullptr)
	{
		::WaitForSingleObject(g_threadReady, 2000);
	}

	g_ticker = ::CreateThread(nullptr, 0, Ticker, nullptr, 0, nullptr);
}

void KbdBridge::Stop()
{
	if (g_thread == nullptr)
	{
		return;
	}

	g_enabled = false;
	ClearQueue();
	ReleaseModifiers();

	::SetEvent(g_stopEvent);
	::PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);

	::WaitForSingleObject(g_thread, 3000);
	::WaitForSingleObject(g_ticker, 3000);
	::WaitForSingleObject(g_relayThread, 3000);

	::CloseHandle(g_relayThread);
	g_relayThread = nullptr;

	::CloseHandle(g_relayEvent);
	g_relayEvent = nullptr;

	g_queueReady = false;
	::DeleteCriticalSection(&g_queueLock);

	::CloseHandle(g_thread);
	g_thread = nullptr;

	::CloseHandle(g_ticker);
	g_ticker = nullptr;

	if (g_threadReady != nullptr)
	{
		::CloseHandle(g_threadReady);
		g_threadReady = nullptr;
	}

	if (g_stopEvent != nullptr)
	{
		::CloseHandle(g_stopEvent);
		g_stopEvent = nullptr;
	}
}

bool KbdBridge::IsEnabled()
{
	return g_enabled.load();
}

void KbdBridge::SetEnabled(bool enabled)
{
	g_enabled = enabled;

	if (g_thread != nullptr && g_threadId != 0)
	{
		::PostThreadMessageW(g_threadId, MSG_SET_ENABLED, enabled ? 1 : 0, 0);
	}
}

void KbdBridge::SetAllowInjected(bool allow)
{
	g_allowInjected = allow;
}

void KbdBridge::SetForcePost(bool force)
{
	g_forcePost = force;
}
