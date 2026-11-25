#pragma once

#include <string>

#include <Windows.h>

// 外部程序启动与图标获取
namespace Launcher
{
	// 查找 Google Chrome，找不到返回空字符串
	std::wstring FindChrome();
	// 查找网易 UU 远程（GameViewer.exe），找不到返回空字符串
	std::wstring FindGameViewer();
	// 打开指定程序，成功返回 true
	bool Open(const std::wstring& path);
	// 取程序的 32x32 图标，失败返回 nullptr。返回的图标不要销毁，随进程结束释放
	HICON LoadIcon32(const std::wstring& path);
}
