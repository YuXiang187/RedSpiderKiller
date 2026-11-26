#pragma once

#include <windows.h>

// 键盘桥：红蜘蛛广播时会在用户态装一个全局 WH_KEYBOARD_LL 钩子把键盘锁死。
// 锁既然在用户态，就能在用户态抢回来：
//
//   1. 我们也装一个 WH_KEYBOARD_LL，并且每 300ms 重装一次，保证排在钩子链首
//      （链首先被调用，能拿到事件；被吞掉的键连 GetAsyncKeyState 都不更新，
//       所以「轮询读键」这条退路不存在，只有抢链首一条路）
//   2. 抢到事件以后有两条路把键送进目标程序：
//
//   a) 注入（默认）：SendInput 把按键原样塞回系统输入队列。
//      对方的钩子只吞「物理」按键，对它自己也用得着的注入事件是放行的，
//      所以这条路能穿过去。走的是正常输入栈，所以输入法（含 Shift 切中英文、
//      候选词）、退格、方向键、Ctrl/Alt 快捷键、按住自动重复全部和真键盘一致。
//
//   b) 投递（兜底）：PostMessage(WM_CHAR/WM_KEYDOWN) 直接塞进目标窗口的消息队列。
//      只在目标进程权限比我们高（UIPI 会把注入静默丢掉）时使用。这条路绕过
//      输入法，只能出 ASCII，中文打不出来。
//
// 刻意不做的部分：
//   - 提权目标窗口的中文：注入被 UIPI 拦，投消息又绕不过输入法
//   - UWP 应用：不吃投递的消息，注入模式可以，投递模式不行
namespace KbdBridge
{
	// 启动/停止后台线程（只起线程，不装钩子）
	void Start();
	void Stop();

	bool IsEnabled();
	void SetEnabled(bool enabled);

	// 仅自动化测试用：把注入进来的按键也当成物理键处理，
	// 这样测试程序可以用 SendInput 模拟打字。正常使用保持关闭
	void SetAllowInjected(bool allow);

	// 强制退回投递模式，不走注入（排查注入有问题时用，也可用 --kbd-post 指定）
	void SetForcePost(bool force);
}
