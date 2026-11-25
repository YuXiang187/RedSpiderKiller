#pragma once

#include <string>

// 调出 Ctrl+Alt+Del 的安全桌面。
//
// 为什么不能直接模拟按键：SAS 是在 win32k 的原始输入路径里判定的，键盘过滤驱动
// （UpperFilters，比如极域的 TDKeybd、机房的 ezkbfiltr）在它下面，物理按键会先被吃掉；
// SendInput 注入的 Ctrl+Alt+Del 又会被 win32k 直接丢弃。唯一能穿过整条键盘栈的是
// sas.dll 的 SendSAS —— 它直接走 Winlogon 的 SAS 通道。
//
// SendSAS 的两个硬前提：
//   1. HKLM\...\Policies\System\SoftwareSASGeneration >= 1
//   2. 调用进程必须持有 SeTcbPrivilege，也就是必须跑在 SYSTEM 身份下
// 所以流程是：提权 -> 写策略 -> 用 winlogon 的令牌起一个 SYSTEM 子进程 -> 子进程调 SendSAS。
namespace SecureDesktop
{
	// 命令行里是否带了指定开关（用 CommandLineToArgvW 解析，不依赖 lpCmdLine）
	bool HasSwitch(const wchar_t* name);

	// 当前进程是否已经提权
	bool IsElevated();

	// 当前进程的映像路径
	std::wstring GetSelfPath();

	// 工具栏按钮入口。没提权时用 runas 拉起自己做，失败原因写进 error
	bool Trigger(std::wstring& error);

	// --secure-desktop：提权后的实例，做完事情自己报错，返回进程退出码
	int RunElevatedInstance();

	// --send-sas：SYSTEM 实例，真正调用 SendSAS，返回进程退出码
	// 0 = 调用完成，2 = sas.dll 加载失败，3 = 找不到导出函数，4 = 权限不足
	int RunSystemInstance();
}
