#include "stdafx.h"
#include "Launcher.h"

#include <shellapi.h>
#include <vector>

namespace
{
	bool FileExists(const std::wstring& path)
	{
		if (path.empty())
		{
			return false;
		}

		auto attr = ::GetFileAttributes(path.c_str());

		return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
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

	// 环境变量 + 相对路径，拼不出（变量不存在）时返回空字符串
	std::wstring JoinEnvironment(const wchar_t* environmentName, const wchar_t* relativePath)
	{
		wchar_t buffer[MAX_PATH] = { 0 };
		if (::GetEnvironmentVariable(environmentName, buffer, _countof(buffer)) == 0)
		{
			return std::wstring();
		}

		return std::wstring(buffer) + L"\\" + relativePath;
	}

	// 从 App Paths 读取程序位置。Chrome 安装时会写这里，跟装在哪个盘无关
	std::wstring ReadAppPath(const std::wstring& exeName)
	{
		const HKEY roots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
		const wchar_t* bases[] =
		{
			L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\",
			L"SOFTWARE\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths\\",
		};

		for (auto root : roots)
		{
			for (auto base : bases)
			{
				auto subPath = std::wstring(base) + exeName;

				HKEY key = nullptr;
				if (::RegOpenKeyEx(root, subPath.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
				{
					continue;
				}

				wchar_t buffer[MAX_PATH * 2] = { 0 };
				DWORD size = sizeof(buffer);
				DWORD type = 0;
				auto status = ::RegQueryValueEx(key, nullptr, nullptr, &type,
					reinterpret_cast<LPBYTE>(buffer), &size);
				::RegCloseKey(key);

				if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
				{
					continue;
				}

				std::wstring value = buffer;
				if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
				{
					value = value.substr(1, value.size() - 2);
				}

				if (FileExists(value))
				{
					return value;
				}
			}
		}

		return std::wstring();
	}

	std::wstring FirstExisting(const std::vector<std::wstring>& candidates)
	{
		for (const auto& path : candidates)
		{
			if (FileExists(path))
			{
				return path;
			}
		}

		return std::wstring();
	}
}

namespace Launcher
{
	std::wstring FindChrome()
	{
		std::vector<std::wstring> candidates;

		auto fromRegister = ReadAppPath(L"chrome.exe");
		if (!fromRegister.empty())
		{
			candidates.push_back(fromRegister);
		}

		// 64 位系统下 32 位进程读到的 ProgramFiles 是 x86 目录，所以 ProgramW6432 也补上
		candidates.push_back(JoinEnvironment(L"ProgramW6432", L"Google\\Chrome\\Application\\chrome.exe"));
		candidates.push_back(JoinEnvironment(L"ProgramFiles", L"Google\\Chrome\\Application\\chrome.exe"));
		candidates.push_back(JoinEnvironment(L"ProgramFiles(x86)", L"Google\\Chrome\\Application\\chrome.exe"));
		candidates.push_back(JoinEnvironment(L"LocalAppData", L"Google\\Chrome\\Application\\chrome.exe"));
		candidates.push_back(L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe");
		candidates.push_back(L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe");

		return FirstExisting(candidates);
	}

	std::wstring FindGameViewer()
	{
		std::vector<std::wstring> candidates;

		candidates.push_back(JoinEnvironment(L"ProgramW6432", L"Netease\\GameViewer\\GameViewer.exe"));
		candidates.push_back(JoinEnvironment(L"ProgramFiles", L"Netease\\GameViewer\\GameViewer.exe"));
		candidates.push_back(JoinEnvironment(L"ProgramFiles(x86)", L"Netease\\GameViewer\\GameViewer.exe"));
		candidates.push_back(L"C:\\Program Files\\Netease\\GameViewer\\GameViewer.exe");
		candidates.push_back(L"C:\\Program Files (x86)\\Netease\\GameViewer\\GameViewer.exe");

		return FirstExisting(candidates);
	}

	bool Open(const std::wstring& path)
	{
		if (!FileExists(path))
		{
			return false;
		}

		// 工作目录给程序所在目录，个别程序会依赖它
		auto directory = GetDirectory(path);
		auto result = reinterpret_cast<INT_PTR>(::ShellExecute(nullptr, L"open", path.c_str(),
			nullptr, directory.empty() ? nullptr : directory.c_str(), SW_SHOWNORMAL));

		return result > 32;
	}

	HICON LoadIcon32(const std::wstring& path)
	{
		if (!FileExists(path))
		{
			return nullptr;
		}

		SHFILEINFO info = { 0 };
		if (::SHGetFileInfo(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) == 0)
		{
			return nullptr;
		}

		return info.hIcon;
	}
}
