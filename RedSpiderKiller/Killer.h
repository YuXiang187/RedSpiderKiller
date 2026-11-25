#pragma once

#include <memory>
#include <string>
#include <vector>

#include <Windows.h>

// 本程序适配本机安装的课堂管理 / 屏幕监控软件：把它们的全屏广播窗口塞进自己的顶层窗口，
// 这样不结束学生端进程（教师端不会报警），也可以随时隐藏 / 恢复屏幕内容。
//
// 已适配的产品与各自的识别规则（除识别规则外，其余逻辑与产品无关）：
//   红蜘蛛 7.2.1720：REDAgent.exe
//                    注册表 HKLM\SOFTWARE\3000soft\Red Spider 的 AgentCommand
//                    （本程序是 32 位，系统会自动重定向到 Wow6432Node）
//   Veyon          ：veyon-worker.exe / veyon-server.exe
//                    服务 VeyonService 的 ImagePath 所在目录
//   极域 V6.0 2016 ：StudentMain.exe
//                    %ProgramFiles(x86)%\Mythware\<版本目录>\StudentMain.exe
//                    （版本目录名带产品版本号，按子目录找，避免写死中文目录名）
//
// 三款软件同时装了也没关系：识别到几款就同时支持几款，谁在全屏广播就窗口化谁。
class Killer
	: public std::enable_shared_from_this<Killer>
{
public:
	static std::shared_ptr<Killer> Create();

private:
	Killer() = default;

public:
	~Killer();

public:
	// 初始化：找出本机装了哪些学生端，一款都没找到时返回 false
	bool Init();
	// 窗口化
	void Window();
	// 隐藏（当做强制结束，毕竟结束掉教师端会提示）
	void Hide();
	// 隐藏之后重新显示
	void Show();

	// 设置包含窗口，不需要 Init
	// @param window：要设置的窗口句柄
	void SetTopWindow(HWND window);
	// 自动捕获学生端窗口：枚举所有属于学生端、可见、且覆盖绝大部分屏幕的窗口
	// @return：捕获到至少一个窗口时返回 true
	bool AcquireAgentWindow();

	// 手动设置学生端路径，不需要 Init
	// @param filePath：可执行文件的完整路径
	void SetFilePath(const std::wstring& filePath);

	bool IsHide();
	bool IsInited();
	// 复查已经捕获的窗口：学生端重建窗口、或者把窗口从包含窗口里抢回桌面时，
	// 重新塞回包含窗口。已经一个都不剩（学生端重启）时返回 false
	bool RefreshAgentWindows();
	// 学生端是否还在显示（它自己把广播窗口收起来之后返回 false）
	bool IsAgentWindowShowing() const;
	// 广播结束：把学生端窗口还给系统并收起包含窗口，把桌面露出来
	void CloseAgentWindow();
	// 已识别到的学生端名称，多个用「、」连接，未识别时为空
	std::wstring GetAgentNames() const;
	// 自动识别到的学生端可执行文件完整路径，未识别时为空字符串
	const std::wstring& GetFilePath() const;

private:
	// 一款学生端
	struct Agent
	{
		// 显示名（红蜘蛛 / Veyon / 极域）
		std::wstring name;
		// 可执行文件完整路径，认不出来时为空
		std::wstring imagePath;
		// 安装目录，认不出来时为空
		std::wstring installDir;
	};

	// 枚举窗口的回调
	static BOOL CALLBACK EnumAgentWindowProc(HWND window, LPARAM param);

	// 找出本机在用的学生端，找到至少一款返回 true
	bool LoadAgents();
	// 记录一款学生端，重复的忽略
	void AddAgent(const std::wstring& name, const std::wstring& imagePath);
	// 文件存在时才记录
	void AddAgentIfExists(const std::wstring& name, const std::wstring& imagePath);
	// 是否已经识别到某一款
	bool HasAgent(const std::wstring& name) const;
	// 判断某个映像路径是否属于已识别的学生端
	bool IsAgentImage(const std::wstring& imagePath) const;
	// 记录一个符合条件的学生端窗口
	void CollectAgentWindow(HWND window);
	// 把学生端窗口挂到包含窗口上。已经挂好的直接跳过：
	// SetParent 会把窗口重新插入父窗口的子链表并触发一次重绘，
	// 对同一个窗口反复调用就是全屏广播画面闪烁的根源
	void AttachWindow(HWND window);
	// 丢掉已经销毁的窗口
	void PruneDeadAgentWindows();
	// 把捕获到的窗口还给系统
	void ReleaseAgentWindows();

private:
	// 命中本机的学生端
	std::vector<Agent> m_agents;
	// 已捕获的学生端窗口：极域会同时开两块全屏广播窗口，所以是多个
	std::vector<HWND> m_agentWindows;

	// 用于包含学生端窗口的窗口
	HWND m_topWindow = nullptr;

	bool m_inited = false;
	bool m_hided = true;
};
