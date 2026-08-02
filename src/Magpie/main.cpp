// Copyright (c) Xu
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.


#include "pch.h"
#include "StrHelper.h"
#include "App.h"
#include "Win32Helper.h"
#include "TouchHelper.h"
#include "CommonSharedConstants.h"
#include "Logger.h"

using namespace Magpie;
using namespace winrt::Magpie::implementation;

// 将当前目录设为程序所在目录
static void SetWorkingDir() noexcept {
	const std::filesystem::path exeDir = Win32Helper::GetExePath().parent_path();
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetCurrentDirectory(exeDir.c_str()));

	// onnxruntime / DirectML / cudart 从 third_party 延迟加载
	// onnxruntime, DirectML and cudart are delay-loaded out of third_party\.
	// Without this the first delay-load - cudaD3D11GetDevice inside
	// TensorRTInferenceBackend::Initialize - raises and kills the process.
	SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	const std::wstring thirdPartyDir = (exeDir / L"third_party").wstring();
	AddDllDirectory(thirdPartyDir.c_str());

	// Windows 自带 onnxruntime.dll（System32），且它的搜索顺序在
	// AddDllDirectory 之前，因此必须用绝对路径先加载我们自己的副本。
	//
	// System32 holds the OS copy of ONNX Runtime and is searched before any
	// AddDllDirectory path, so an unqualified load binds that one. Built
	// against newer headers, GetApi(ORT_API_VERSION) then returns nullptr and
	// the first Ort call dereferences null. Pin ours by absolute path; later
	// resolutions of the same name reuse this module.
	for (const wchar_t* dllName : { L"onnxruntime.dll", L"DirectML.dll" }) {
		const std::wstring dllPath = thirdPartyDir + L"\\" + dllName;
		if (!LoadLibraryEx(dllPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
			LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)) {
			// 不致命：没有 ONNX 时缩放仍可工作
			// Not fatal - scaling still works without ONNX.
			Logger::Get().Win32Error(StrHelper::Concat(
				"加载失败 / failed to preload ", StrHelper::UTF16ToUTF8(dllPath)));
		}
	}
}

static void InitializeLogger(const wchar_t* logFilePath) noexcept {
	// 最多两个日志文件，每个最多 500KB
	Logger::Get().Initialize(
		spdlog::level::info,
		logFilePath,
		CommonSharedConstants::LOG_MAX_SIZE,
		1
	);
}

int APIENTRY wWinMain(
	_In_ HINSTANCE /*hInstance*/,
	_In_opt_ HINSTANCE /*hPrevInstance*/,
	_In_ wchar_t* lpCmdLine,
	_In_ int /*nCmdShow*/
) {
#ifdef _DEBUG
	SetThreadDescription(GetCurrentThread(), L"Magpie-主线程");
#endif
	
	// 堆损坏时终止进程
	HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, nullptr, 0);

	SetWorkingDir();

	enum {
		Normal,
		RegisterTouchHelper,
		UnRegisterTouchHelper
	} mode = [&]() {
		if (lpCmdLine == L"-r"sv) {
			return RegisterTouchHelper;
		} else if (lpCmdLine == L"-ur"sv) {
			return UnRegisterTouchHelper;
		} else {
			return Normal;
		}
	}();

	InitializeLogger(mode == Normal ?
		CommonSharedConstants::LOG_PATH :
		CommonSharedConstants::REGISTER_TOUCH_HELPER_LOG_PATH);

	Logger::Get().Info(fmt::format("程序启动\n\t版本: {}\n\tOS 版本: {}\n\t管理员: {}",
#ifdef MP_VERSION_STRING
		STRINGIFY(MP_VERSION_STRING),
#elif defined(MP_COMMIT_ID)
		"dev (" STRINGIFY(MP_COMMIT_ID) ")",
#else
		"dev",
#endif
		Win32Helper::GetOSVersion().ToString<char>(),
		Win32Helper::IsProcessElevated() ? "是" : "否"
	));

	if (mode == RegisterTouchHelper) {
		// 使 TouchHelper 获得 UIAccess 权限
		return Magpie::TouchHelper::Register() ? 0 : 1;
	} else if (mode == UnRegisterTouchHelper) {
		return Magpie::TouchHelper::Unregister() ? 0 : 1;
	}

	// 程序结束时也不应调用 uninit_apartment
	// 见 https://kennykerr.ca/2018/03/24/cppwinrt-hosting-the-windows-runtime/
	winrt::init_apartment(winrt::apartment_type::single_threaded);

	auto& app = App::Get();
	if (!app.Initialize(lpCmdLine)) {
		return 0;
	}

	return app.Run();
}
