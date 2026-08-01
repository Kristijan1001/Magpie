#pragma once
#include <functional>
#include <string>

namespace Magpie {

// Magpie.Core 向应用层报告 ONNX 状态的钩子。
//
// The app layer installs Callback; Magpie.Core calls Report from the scaling
// thread. Kept in include/ because OnnxEffectDrawer.h is internal to
// Magpie.Core and not visible to src/Magpie.
//
// Magpie.Core is a static library linked into Magpie.exe, so a plain
// std::function is enough - no cross-module event plumbing is needed.
struct OnnxStatus {
	// 在缩放线程上调用，实现必须自行处理线程同步
	// Invoked on the scaling thread; the implementation must marshal.
	static inline std::function<void(std::wstring, std::wstring)> Callback;

	static void Report(std::wstring title, std::wstring text) noexcept {
		if (Callback) {
			Callback(std::move(title), std::move(text));
		}
	}
};

}
