#include "stdafx.h"
#include "SecureDesktop.h"

#include <tlhelp32.h>
#include <shellapi.h>

namespace
{
	const wchar_t* const POLICY_SUB_KEY = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
	const wchar_t* const POLICY_VALUE_NAME = L"SoftwareSASGeneration";

	// mingw.org 的 w32api 里没有 CreateProcessWithTokenW（原型和 import 库都缺），
	// 只能运行时从 advapi32.dll 取地址
	typedef BOOL (WINAPI* FnCreateProcessWithTokenW)(HANDLE, DWORD, LPCWSTR, LPWSTR, DWORD,
		LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);

	typedef VOID (WINAPI* FnSendSAS)(BOOL);

	std::wstring Describe(const wchar_t* what, DWORD code)
	{
		wchar_t buffer[256] = { 0 };
		_snwprintf(buffer, 255, L"%s（错误码 %lu）", what, code);

		return std::wstring(buffer);
	}

	bool EnablePrivilege(const wchar_t* name)
	{
		HANDLE token = nullptr;
		if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
		{
			return false;
		}

		TOKEN_PRIVILEGES privileges = {};
		privileges.PrivilegeCount = 1;

		bool ok = false;
		if (::LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid))
		{
			privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
			BOOL adjusted = ::AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
			ok = adjusted != FALSE && ::GetLastError() == ERROR_SUCCESS;
		}

		::CloseHandle(token);

		return ok;
	}

	// 放开 SoftwareSASGeneration。已经放开过（1/2/3）就不动
	bool EnsureSasPolicy(std::wstring& error)
	{
		HKEY key = nullptr;
		LONG status = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, POLICY_SUB_KEY, 0, nullptr,
			REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
		if (status != ERROR_SUCCESS)
		{
			error = Describe(L"无法打开策略注册表，需要管理员权限", status);

			return false;
		}

		DWORD value = 0;
		DWORD size = sizeof(value);
		DWORD type = 0;

		bool needWrite = true;
		if (::RegQueryValueExW(key, POLICY_VALUE_NAME, nullptr, &type,
			reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS && type == REG_DWORD)
		{
			// 1 = 只允许服务，2 = 只允许应用，3 = 都允许
			needWrite = (value < 1);
		}

		if (needWrite)
		{
			DWORD one = 1;
			status = ::RegSetValueExW(key, POLICY_VALUE_NAME, 0, REG_DWORD,
				reinterpret_cast<const BYTE*>(&one), sizeof(one));
		}

		::RegCloseKey(key);

		if (status != ERROR_SUCCESS)
		{
			error = Describe(L"写入 SoftwareSASGeneration 失败", status);

			return false;
		}

		return true;
	}

	DWORD FindWinlogonInSession(DWORD sessionId)
	{
		HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE)
		{
			return 0;
		}

		DWORD found = 0;
		PROCESSENTRY32W entry = { 0 };
		entry.dwSize = sizeof(entry);

		if (::Process32FirstW(snapshot, &entry))
		{
			do
			{
				if (::lstrcmpiW(entry.szExeFile, L"winlogon.exe") != 0)
				{
					continue;
				}

				DWORD owner = 0;
				if (::ProcessIdToSessionId(entry.th32ProcessID, &owner) && owner == sessionId)
				{
					found = entry.th32ProcessID;
					break;
				}
			} while (::Process32NextW(snapshot, &entry));
		}

		::CloseHandle(snapshot);

		return found;
	}

	// 用当前会话里 winlogon 的令牌，在本会话起一个 SYSTEM 子进程
	bool RunSelfAsSystem(const std::wstring& exePath, const std::wstring& arguments,
		DWORD& exitCode, std::wstring& error)
	{
		EnablePrivilege(L"SeDebugPrivilege");
		EnablePrivilege(L"SeImpersonatePrivilege");

		DWORD sessionId = 0;
		::ProcessIdToSessionId(::GetCurrentProcessId(), &sessionId);

		DWORD winlogonPid = FindWinlogonInSession(sessionId);
		if (winlogonPid == 0)
		{
			error = L"当前会话里找不到 winlogon.exe。";

			return false;
		}

		HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, winlogonPid);
		if (process == nullptr)
		{
			error = Describe(L"打开 winlogon 失败", ::GetLastError());

			return false;
		}

		HANDLE token = nullptr;
		BOOL ok = ::OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY, &token);
		::CloseHandle(process);

		if (!ok)
		{
			error = Describe(L"打开 winlogon 令牌失败", ::GetLastError());

			return false;
		}

		HANDLE primary = nullptr;
		ok = ::DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primary);
		::CloseHandle(token);

		if (!ok)
		{
			error = Describe(L"复制 SYSTEM 令牌失败", ::GetLastError());

			return false;
		}

		HMODULE advapi = ::LoadLibraryW(L"advapi32.dll");
		FnCreateProcessWithTokenW createWithToken = (advapi != nullptr)
			? reinterpret_cast<FnCreateProcessWithTokenW>(::GetProcAddress(advapi, "CreateProcessWithTokenW"))
			: nullptr;

		if (createWithToken == nullptr)
		{
			::CloseHandle(primary);
			error = L"系统里找不到 CreateProcessWithTokenW。";

			return false;
		}

		std::wstring commandLine = L"\"" + exePath + L"\" " + arguments;

		STARTUPINFOW startup = { sizeof(startup) };
		startup.dwFlags = STARTF_USESHOWWINDOW;
		startup.wShowWindow = SW_HIDE;

		PROCESS_INFORMATION info = {};
		ok = createWithToken(primary, 0, nullptr, &commandLine[0], CREATE_NO_WINDOW,
			nullptr, nullptr, &startup, &info);
		DWORD last = ::GetLastError();
		::CloseHandle(primary);

		if (!ok)
		{
			error = Describe(L"以 SYSTEM 身份启动失败", last);

			return false;
		}

		::WaitForSingleObject(info.hProcess, 5000);

		exitCode = 0;
		::GetExitCodeProcess(info.hProcess, &exitCode);

		::CloseHandle(info.hThread);
		::CloseHandle(info.hProcess);

		return true;
	}

	// 真正生成 SAS。必须在 SYSTEM 身份下调用
	int SendSasOnce()
	{
		// SYSTEM 令牌里 SeTcbPrivilege 默认是禁用状态，先打开
		EnablePrivilege(L"SeTcbPrivilege");

		HMODULE sas = ::LoadLibraryW(L"sas.dll");
		if (sas == nullptr)
		{
			return 2;
		}

		FnSendSAS sendSas = reinterpret_cast<FnSendSAS>(::GetProcAddress(sas, "SendSAS"));
		if (sendSas == nullptr)
		{
			::FreeLibrary(sas);

			return 3;
		}

		// FALSE = 按"服务生成"处理，对应策略值 1。
		// 如果机器上策略是 2（只允许应用），这里要改成 TRUE
		sendSas(FALSE);

		::FreeLibrary(sas);

		return 0;
	}

	int RunElevated(std::wstring& error)
	{
		if (!EnsureSasPolicy(error))
		{
			return 10;
		}

		DWORD exitCode = 0;
		if (!RunSelfAsSystem(SecureDesktop::GetSelfPath(), L"--send-sas", exitCode, error))
		{
			return 11;
		}

		if (exitCode != 0)
		{
			wchar_t buffer[256] = { 0 };
			_snwprintf(buffer, 255, L"SYSTEM 子进程返回 %lu：", exitCode);

			error = std::wstring(buffer);
			switch (exitCode)
			{
			case 2:
				error += L"加载 sas.dll 失败。";
				break;
			case 3:
				error += L"sas.dll 里没有 SendSAS 导出。";
				break;
			default:
				error += L"SendSAS 没有成功执行。";
				break;
			}

			return 12;
		}

		return 0;
	}
}

bool SecureDesktop::HasSwitch(const wchar_t* name)
{
	int argc = 0;
	LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
	if (argv == nullptr)
	{
		return false;
	}

	bool found = false;
	for (int i = 1; i < argc && !found; ++i)
	{
		if (::lstrcmpiW(argv[i], name) == 0)
		{
			found = true;
		}
	}

	::LocalFree(argv);

	return found;
}

bool SecureDesktop::IsElevated()
{
	HANDLE token = nullptr;
	if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
	{
		return false;
	}

	// TOKEN_ELEVATION，mingw 的 w32api 里没有，用数值 + 本地结构
	struct ElevationInfo { DWORD TokenIsElevated; };
	ElevationInfo info = {};
	DWORD size = 0;

	BOOL ok = ::GetTokenInformation(token, static_cast<TOKEN_INFORMATION_CLASS>(20),
		&info, sizeof(info), &size);
	::CloseHandle(token);

	return ok != FALSE && info.TokenIsElevated != 0;
}

std::wstring SecureDesktop::GetSelfPath()
{
	wchar_t buffer[MAX_PATH * 2] = { 0 };
	DWORD length = ::GetModuleFileNameW(nullptr, buffer, _countof(buffer));
	if (length == 0 || length >= _countof(buffer))
	{
		return std::wstring();
	}

	return std::wstring(buffer, length);
}

bool SecureDesktop::Trigger(std::wstring& error)
{
	if (!IsElevated())
	{
		// 没提权：用 runas 拉起自己的副本去做，由副本自己报错
		std::wstring self = GetSelfPath();
		if (self.empty())
		{
			error = L"取不到自身路径。";

			return false;
		}

		SHELLEXECUTEINFOW info = { sizeof(info) };
		info.fMask = SEE_MASK_FLAG_NO_UI;
		info.lpVerb = L"runas";
		info.lpFile = self.c_str();
		info.lpParameters = L"--secure-desktop";
		info.nShow = SW_HIDE;

		if (!::ShellExecuteExW(&info))
		{
			error = Describe(L"申请管理员权限失败", ::GetLastError());

			return false;
		}

		return true;
	}

	return RunElevated(error) == 0;
}

int SecureDesktop::RunElevatedInstance()
{
	std::wstring error;
	int code = RunElevated(error);

	if (code != 0)
	{
		::MessageBoxW(nullptr, error.c_str(), L"安全桌面", MB_OK | MB_ICONWARNING);
	}

	return code;
}

int SecureDesktop::RunSystemInstance()
{
	return SendSasOnce();
}
