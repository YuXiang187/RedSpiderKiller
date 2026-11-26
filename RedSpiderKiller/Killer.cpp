#include "stdafx.h"
#include "Killer.h"

#include <tlhelp32.h>

#define ENSURE_INITED if (!m_inited) return

namespace
{
	// 本机红蜘蛛 7.2.1720 的固定位置
	const wchar_t* const REDSPIDER_REGISTER_SUB_PATH = L"SOFTWARE\\3000soft\\Red Spider";
	const wchar_t* const REDSPIDER_REGISTER_VALUE_NAME = L"AgentCommand";
	const wchar_t* const REDSPIDER_AGENT_IMAGE = L"C:\\Program Files (x86)\\3000soft\\Red Spider\\REDAgent.exe";

	// Veyon 的系统服务，ImagePath 指向安装目录里的 veyon-service.exe
	const wchar_t* const VEYON_SERVICE_SUB_PATH = L"SYSTEM\\CurrentControlSet\\Services\\VeyonService";
	const wchar_t* const VEYON_SERVICE_VALUE_NAME = L"ImagePath";

	// 屏幕广播窗口至少要覆盖 80% 的屏幕，用来把广播窗口跟提示框、小工具窗区分开
	const int FULLSCREEN_PERCENT = 80;

	// 会在当前会话里持有全屏广播窗口的进程，以及它属于哪款产品。
	// 只按映像名识别，跟装在哪个目录、哪个版本无关。
	struct KnownProcess
	{
		const wchar_t* fileName;
		const wchar_t* agentName;
	};

	const KnownProcess KNOWN_PROCESSES[] =
	{
		{ L"REDAgent.exe", L"红蜘蛛" },
		{ L"veyon-worker.exe", L"Veyon" },
		{ L"veyon-server.exe", L"Veyon" },
		{ L"StudentMain.exe", L"极域" },
	};

	// 不区分大小写的字符串比较
	bool EqualsNoCase(const std::wstring& left, const std::wstring& right)
	{
		if (left.size() != right.size())
		{
			return false;
		}

		if (left.empty())
		{
			return true;
		}

		return ::CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
			right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
	}

	bool EndsWithNoCase(const std::wstring& value, const std::wstring& tail)
	{
		if (value.size() < tail.size())
		{
			return false;
		}

		return EqualsNoCase(value.substr(value.size() - tail.size()), tail);
	}

	std::wstring Trim(const std::wstring& value)
	{
		auto begin = value.find_first_not_of(L" \t\r\n");
		if (begin == std::wstring::npos)
		{
			return std::wstring();
		}

		auto end = value.find_last_not_of(L" \t\r\n");

		return value.substr(begin, end - begin + 1);
	}

	// 规整注册表里的路径：去掉空白、引号，以及可执行文件后面的命令行参数
	std::wstring NormalizePath(const std::wstring& raw)
	{
		auto value = Trim(raw);

		// "C:\...\REDAgent.exe" 这种带引号的形式
		if (!value.empty() && value.front() == L'"')
		{
			auto close = value.find(L'"', 1);
			value = (close == std::wstring::npos) ? value.substr(1) : value.substr(1, close - 1);
		}

		// 本机注册表里存的是 "C:\...\REDAgent.exe "（尾部带空格），这里统一截到 .exe
		auto exe = value.rfind(L".exe");
		if (exe != std::wstring::npos && !EndsWithNoCase(value, L".exe"))
		{
			auto tail = value.substr(exe + 4);
			if (!tail.empty() && (tail.front() == L' ' || tail.front() == L'\t' || tail.front() == L'/'))
			{
				value = value.substr(0, exe + 4);
			}
		}

		return Trim(value);
	}

	std::wstring GetDirectory(const std::wstring& path)
	{
		auto pos = path.find_last_of(L"\\/");
		if (pos == std::wstring::npos)
		{
			return std::wstring();
		}

		return path.substr(0, pos);
	}

	std::wstring GetFileName(const std::wstring& path)
	{
		auto pos = path.find_last_of(L"\\/");
		if (pos == std::wstring::npos)
		{
			return path;
		}

		return path.substr(pos + 1);
	}

	bool IsFileExists(const std::wstring& path)
	{
		if (path.empty())
		{
			return false;
		}

		WIN32_FIND_DATA nextInfo;
		auto file = ::FindFirstFile(path.c_str(), &nextInfo);
		if (file == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		::FindClose(file);

		return (nextInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	bool IsDirectoryExists(const std::wstring& path)
	{
		if (path.empty())
		{
			return false;
		}

		auto attr = ::GetFileAttributes(path.c_str());

		return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	std::wstring ExpandEnvironment(const std::wstring& value)
	{
		wchar_t buffer[MAX_PATH * 2] = { 0 };
		auto length = ::ExpandEnvironmentStrings(value.c_str(), buffer, _countof(buffer));
		if (length == 0 || length > _countof(buffer))
		{
			return value;
		}

		return std::wstring(buffer);
	}

	// 读注册表里的字符串（REG_SZ / REG_EXPAND_SZ）
	bool ReadRegistryString(const wchar_t* subPath, const wchar_t* valueName, std::wstring& value)
	{
		HKEY key = nullptr;
		if (::RegOpenKeyEx(HKEY_LOCAL_MACHINE, subPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
		{
			return false;
		}

		DWORD type = 0;
		DWORD size = 0;
		auto status = ::RegQueryValueEx(key, valueName, nullptr, &type, nullptr, &size);
		if (status != ERROR_SUCCESS || size == 0 || (type != REG_SZ && type != REG_EXPAND_SZ))
		{
			::RegCloseKey(key);
			return false;
		}

		std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 2, L'\0');
		status = ::RegQueryValueEx(key, valueName, nullptr, &type,
			reinterpret_cast<LPBYTE>(buffer.data()), &size);
		::RegCloseKey(key);

		if (status != ERROR_SUCCESS)
		{
			return false;
		}

		value.assign(buffer.data());

		if (type == REG_EXPAND_SZ)
		{
			value = ExpandEnvironment(value);
		}

		return !value.empty();
	}

	// 环境变量 + 相对路径。变量不存在时返回空字符串
	std::wstring JoinEnvironment(const wchar_t* environmentName, const wchar_t* relativePath)
	{
		wchar_t buffer[MAX_PATH] = { 0 };
		if (::GetEnvironmentVariable(environmentName, buffer, _countof(buffer)) == 0)
		{
			return std::wstring();
		}

		return std::wstring(buffer) + L"\\" + relativePath;
	}

	// 在 parentDir 的每个子目录里找 leaf，返回第一个存在的完整路径。
	// 极域的安装目录带着产品名和版本号（…\Mythware\<产品目录>\StudentMain.exe），
	// 用这个就不用在源码里写死中文目录名。
	std::wstring FindInSubDirectories(const std::wstring& parentDir, const wchar_t* leaf)
	{
		if (!IsDirectoryExists(parentDir))
		{
			return std::wstring();
		}

		auto mask = parentDir + L"\\*";
		WIN32_FIND_DATA nextInfo;
		auto find = ::FindFirstFile(mask.c_str(), &nextInfo);
		if (find == INVALID_HANDLE_VALUE)
		{
			return std::wstring();
		}

		std::wstring found;
		do
		{
			if ((nextInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			{
				continue;
			}

			if (::wcscmp(nextInfo.cFileName, L".") == 0 || ::wcscmp(nextInfo.cFileName, L"..") == 0)
			{
				continue;
			}

			auto candidate = parentDir + L"\\" + nextInfo.cFileName + L"\\" + leaf;
			if (IsFileExists(candidate))
			{
				found = candidate;
				break;
			}
		} while (::FindNextFile(find, &nextInfo));

		::FindClose(find);

		return found;
	}

	std::wstring GetProcessImagePath(DWORD processId)
	{
		// 学生端跑在用户会话里，普通权限即可读到映像路径
		HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
		if (handle == nullptr)
		{
			return std::wstring();
		}

		wchar_t buffer[MAX_PATH * 2] = { 0 };
		DWORD size = _countof(buffer);
		std::wstring path;

		if (::QueryFullProcessImageName(handle, 0, buffer, &size))
		{
			path.assign(buffer, size);
		}

		::CloseHandle(handle);

		return path;
	}

	// 按映像名找出所有正在运行的进程的完整路径
	std::vector<std::wstring> FindRunningImages(const wchar_t* fileName)
	{
		std::vector<std::wstring> images;

		HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE)
		{
			return images;
		}

		PROCESSENTRY32W entry = { 0 };
		entry.dwSize = sizeof(entry);

		if (::Process32FirstW(snapshot, &entry))
		{
			do
			{
				if (!EqualsNoCase(entry.szExeFile, fileName))
				{
					continue;
				}

				auto image = GetProcessImagePath(entry.th32ProcessID);
				if (!image.empty())
				{
					images.push_back(image);
				}
			} while (::Process32NextW(snapshot, &entry));
		}

		::CloseHandle(snapshot);

		return images;
	}
}

Killer::~Killer()
{
	ReleaseAgentWindows();
}

std::shared_ptr<Killer> Killer::Create()
{
	return std::shared_ptr<Killer>(new Killer());
}

bool Killer::Init()
{
	if (!LoadAgents())
	{
		m_inited = false;
		return false;
	}

	m_inited = true;

	return true;
}

void Killer::Window()
{
	ENSURE_INITED;

	// 没有设置包含窗口或者学生端窗口
	if (!::IsWindow(m_topWindow))
	{
		return;
	}

	for (auto window : m_agentWindows)
	{
		AttachWindow(window);
	}
}

void Killer::AttachWindow(HWND window)
{
	if (!::IsWindow(window) || !::IsWindow(m_topWindow))
	{
		return;
	}

	// 已经在包含窗口里，就不要再动它。重复 SetParent 会让广播窗口重新插入
	// 父窗口的子链表、重新计算可见区域，每做一次就闪一下
	if (::GetAncestor(window, GA_PARENT) == m_topWindow)
	{
		return;
	}

	::SetParent(window, m_topWindow);
}

void Killer::Hide()
{
	ENSURE_INITED;

	::ShowWindow(m_topWindow, SW_HIDE);

	m_hided = true;
}

void Killer::Show()
{
	ENSURE_INITED;

	::ShowWindow(m_topWindow, SW_SHOW);

	m_hided = false;
}

void Killer::SetTopWindow(HWND window)
{
	m_topWindow = window;
}

BOOL CALLBACK Killer::EnumAgentWindowProc(HWND window, LPARAM param)
{
	auto self = reinterpret_cast<Killer*>(param);
	if (self == nullptr || !::IsWindowVisible(window))
	{
		return TRUE;
	}

	// 自己的窗口不是学生端窗口，先排除，省得后面白跑一趟取进程路径
	DWORD processId = 0;
	::GetWindowThreadProcessId(window, &processId);
	if (processId == 0 || processId == ::GetCurrentProcessId())
	{
		return TRUE;
	}

	RECT rect = { 0 };
	if (!::GetWindowRect(window, &rect))
	{
		return TRUE;
	}

	auto width = ::GetSystemMetrics(SM_CXSCREEN);
	auto height = ::GetSystemMetrics(SM_CYSCREEN);
	auto windowWidth = rect.right - rect.left;
	auto windowHeight = rect.bottom - rect.top;

	// 至少覆盖 80% 的屏幕才算全屏广播窗口
	if (windowWidth < width * FULLSCREEN_PERCENT / 100 || windowHeight < height * FULLSCREEN_PERCENT / 100)
	{
		return TRUE;
	}

	self->CollectAgentWindow(window);

	return TRUE;
}

bool Killer::AcquireAgentWindow()
{
	ENSURE_INITED false;

	// 保留还活着的旧窗口，只把已经销毁的丢掉，避免重新捕获时窗口闪回桌面
	PruneDeadAgentWindows();

	::EnumWindows(&Killer::EnumAgentWindowProc, reinterpret_cast<LPARAM>(this));

	return !m_agentWindows.empty();
}

bool Killer::RefreshAgentWindows()
{
	ENSURE_INITED false;

	PruneDeadAgentWindows();

	::EnumWindows(&Killer::EnumAgentWindowProc, reinterpret_cast<LPARAM>(this));

	// 学生端有可能自己把窗口的父窗口改回桌面（Qt 重画、MFC 重建窗口等），再塞一次
	if (::IsWindow(m_topWindow))
	{
		for (auto window : m_agentWindows)
		{
			AttachWindow(window);
		}
	}

	return !m_agentWindows.empty();
}

void Killer::SetFilePath(const std::wstring& filePath)
{
	auto path = NormalizePath(filePath);
	auto name = GetFileName(path);

	// 手动选的是已知学生端时用产品名，否则用文件名
	for (const auto& known : KNOWN_PROCESSES)
	{
		if (EqualsNoCase(known.fileName, name))
		{
			name = known.agentName;
			break;
		}
	}

	AddAgent(name, path);

	m_inited = !m_agents.empty();
}

bool Killer::IsHide()
{
	ENSURE_INITED false;

	return m_hided;
}

bool Killer::IsInited()
{
	return m_inited;
}

bool Killer::IsAgentWindowShowing() const
{
	for (auto window : m_agentWindows)
	{
		// 看窗口自己的 WS_VISIBLE：包含窗口被我们隐藏时，子窗口的这个位是保留的，
		// 所以这里判断的是"学生端还想不想显示"，跟我们的显示状态无关
		if ((::GetWindowLong(window, GWL_STYLE) & WS_VISIBLE) != 0)
		{
			return true;
		}
	}

	return false;
}

void Killer::CloseAgentWindow()
{
	ENSURE_INITED;

	// 还给系统，并收起包含窗口：否则容器会留下一整屏空白盖住桌面
	ReleaseAgentWindows();

	if (::IsWindow(m_topWindow))
	{
		::ShowWindow(m_topWindow, SW_HIDE);
	}

	m_hided = true;
}

std::wstring Killer::GetAgentNames() const
{
	std::wstring names;

	for (const auto& agent : m_agents)
	{
		if (agent.name.empty())
		{
			continue;
		}

		if (names.find(agent.name) != std::wstring::npos)
		{
			continue;
		}

		if (!names.empty())
		{
			names += L"、";
		}

		names += agent.name;
	}

	return names;
}

bool Killer::LoadAgents()
{
	m_agents.clear();

	// 先把正在运行的学生端全部认出来：能拿到最准确的路径，跟装在哪个目录无关
	for (const auto& known : KNOWN_PROCESSES)
	{
		for (const auto& image : FindRunningImages(known.fileName))
		{
			AddAgent(known.agentName, image);
		}
	}

	// 学生端没启动时，按各产品的固定位置再找一遍，这样提前开着程序也能用

	// 红蜘蛛
	if (!HasAgent(L"红蜘蛛"))
	{
		std::wstring value;
		if (ReadRegistryString(REDSPIDER_REGISTER_SUB_PATH, REDSPIDER_REGISTER_VALUE_NAME, value))
		{
			AddAgentIfExists(L"红蜘蛛", NormalizePath(value));
		}

		AddAgentIfExists(L"红蜘蛛", REDSPIDER_AGENT_IMAGE);
	}

	// Veyon：服务 ImagePath 指向安装目录，里面的 veyon-worker.exe 就是会话内进程
	if (!HasAgent(L"Veyon"))
	{
		std::wstring value;
		if (ReadRegistryString(VEYON_SERVICE_SUB_PATH, VEYON_SERVICE_VALUE_NAME, value))
		{
			auto installDir = GetDirectory(NormalizePath(value));
			if (!installDir.empty())
			{
				AddAgentIfExists(L"Veyon", installDir + L"\\veyon-worker.exe");
			}
		}

		// Veyon 是 64 位程序，32 位进程读到的 ProgramFiles 是 x86 目录，所以补上 ProgramW6432
		AddAgentIfExists(L"Veyon", JoinEnvironment(L"ProgramW6432", L"Veyon\\veyon-worker.exe"));
		AddAgentIfExists(L"Veyon", JoinEnvironment(L"ProgramFiles", L"Veyon\\veyon-worker.exe"));
	}

	// 极域：安装目录带产品名和版本号，在 Mythware 下面按子目录找 StudentMain.exe
	if (!HasAgent(L"极域"))
	{
		const wchar_t* bases[] = { L"ProgramFiles(x86)", L"ProgramFiles", L"ProgramW6432" };
		for (auto base : bases)
		{
			auto mythwareDir = JoinEnvironment(base, L"Mythware");
			auto studentMain = FindInSubDirectories(mythwareDir, L"StudentMain.exe");
			if (!studentMain.empty())
			{
				AddAgent(L"极域", studentMain);
				break;
			}
		}
	}

	return !m_agents.empty();
}

void Killer::AddAgent(const std::wstring& name, const std::wstring& imagePath)
{
	if (name.empty() || imagePath.empty())
	{
		return;
	}

	for (const auto& agent : m_agents)
	{
		if (EqualsNoCase(agent.imagePath, imagePath))
		{
			return;
		}
	}

	Agent agent;
	agent.name = name;
	agent.imagePath = imagePath;
	agent.installDir = GetDirectory(imagePath);

	m_agents.push_back(agent);
}

void Killer::AddAgentIfExists(const std::wstring& name, const std::wstring& imagePath)
{
	if (IsFileExists(imagePath))
	{
		AddAgent(name, imagePath);
	}
}

bool Killer::HasAgent(const std::wstring& name) const
{
	for (const auto& agent : m_agents)
	{
		if (EqualsNoCase(agent.name, name))
		{
			return true;
		}
	}

	return false;
}

bool Killer::IsAgentImage(const std::wstring& imagePath) const
{
	if (imagePath.empty())
	{
		return false;
	}

	// 跟识别到的完全一致
	for (const auto& agent : m_agents)
	{
		if (EqualsNoCase(imagePath, agent.imagePath))
		{
			return true;
		}
	}

	// 同一安装目录下的进程也算：全屏广播时会从安装目录派生同目录的子进程
	auto directory = GetDirectory(imagePath);
	for (const auto& agent : m_agents)
	{
		if (!agent.installDir.empty() && EqualsNoCase(directory, agent.installDir))
		{
			return true;
		}
	}

	// 兜底：进程名属于已知的学生端（学生端从别的目录启动时也能认出来）
	auto fileName = GetFileName(imagePath);
	for (const auto& known : KNOWN_PROCESSES)
	{
		if (EqualsNoCase(fileName, known.fileName))
		{
			return true;
		}
	}

	return false;
}

void Killer::CollectAgentWindow(HWND window)
{
	DWORD processId = 0;
	::GetWindowThreadProcessId(window, &processId);
	if (processId == 0)
	{
		return;
	}

	auto imagePath = GetProcessImagePath(processId);
	if (!IsAgentImage(imagePath))
	{
		return;
	}

	for (auto exists : m_agentWindows)
	{
		if (exists == window)
		{
			return;
		}
	}

	m_agentWindows.push_back(window);
}

void Killer::PruneDeadAgentWindows()
{
	for (auto it = m_agentWindows.begin(); it != m_agentWindows.end(); )
	{
		if (::IsWindow(*it))
		{
			++it;
		}
		else
		{
			it = m_agentWindows.erase(it);
		}
	}
}

void Killer::ReleaseAgentWindows()
{
	for (auto window : m_agentWindows)
	{
		if (::IsWindow(window))
		{
			::SetParent(window, nullptr);
		}
	}

	m_agentWindows.clear();
}
