#include "pch.h"
#include "OnnxRuntimeService.h"
#include "App.h"
#include "Logger.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>

using namespace winrt;
// App 在 winrt::Magpie::implementation 里，而本文件位于 namespace Magpie
// App lives in winrt::Magpie::implementation; this file is in namespace Magpie,
// so without this the name resolves to Magpie::App and does not exist.
using namespace winrt::Magpie::implementation;
using namespace Windows::Storage::Streams;
using namespace Windows::Web::Http;

namespace Magpie {

// 运行时已经作为 release 资源发布，直接取用，不再另行分发
// The runtime is already published as a release asset, so it is fetched from
// there rather than redistributed again.
static constexpr const wchar_t* RUNTIME_URL =
	L"https://github.com/Blinue/Magpie/releases/download/onnx-preview2/ext-tensorrt-x64.7z";

std::wstring OnnxRuntimeService::_ThirdPartyDir() const noexcept {
	return (Win32Helper::GetExePath().parent_path() / L"third_party").wstring();
}

bool OnnxRuntimeService::IsInstalled() const noexcept {
	const std::wstring probe = _ThirdPartyDir() + L"\\onnxruntime.dll";
	return Win32Helper::FileExists(probe.c_str());
}

void OnnxRuntimeService::_Status(OnnxRuntimeStatus value) {
	if (_status == value) {
		return;
	}

	_status = value;
	StatusChanged.Invoke(value);
}

bool OnnxRuntimeService::_Extract(
	const std::wstring& archivePath,
	const std::wstring& destDir
) noexcept {
	// tar.exe 一定在 System32，用绝对路径避免 PATH 被劫持
	// tar.exe always lives in System32; use the absolute path so a hijacked
	// PATH cannot substitute something else.
	wchar_t systemDir[MAX_PATH];
	const UINT len = GetSystemDirectory(systemDir, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		Logger::Get().Win32Error("GetSystemDirectory 失败");
		return false;
	}

	const std::wstring tarPath = StrHelper::Concat(systemDir, L"\\tar.exe");
	if (!Win32Helper::FileExists(tarPath.c_str())) {
		Logger::Get().Error("找不到 tar.exe / tar.exe is not present");
		return false;
	}

	std::wstring cmdLine = StrHelper::Concat(
		L"\"", tarPath, L"\" -xf \"", archivePath, L"\" -C \"", destDir, L"\"");

	STARTUPINFO si{ .cb = sizeof(si), .dwFlags = STARTF_USESHOWWINDOW, .wShowWindow = SW_HIDE };
	wil::unique_process_information pi;
	if (!CreateProcess(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW, nullptr, nullptr, &si, pi.addressof())) {
		Logger::Get().Win32Error("CreateProcess(tar.exe) 失败");
		return false;
	}

	WaitForSingleObject(pi.hProcess, INFINITE);

	DWORD exitCode = 1;
	GetExitCodeProcess(pi.hProcess, &exitCode);
	if (exitCode != 0) {
		Logger::Get().Error(fmt::format("tar.exe 解压失败，退出码 {}", exitCode));
		return false;
	}

	return true;
}

fire_and_forget OnnxRuntimeService::DownloadAndInstall() {
	if (_status == OnnxRuntimeStatus::Downloading ||
		_status == OnnxRuntimeStatus::Extracting) {
		co_return;
	}

	_cancelled = false;
	_downloadProgress = 0;

	const std::wstring thirdPartyDir = _ThirdPartyDir();
	const std::wstring archivePath = thirdPartyDir + L"\\ext-runtime.7z";

	if (!Win32Helper::DirExists(thirdPartyDir.c_str()) &&
		!Win32Helper::CreateDir(thirdPartyDir, true)) {
		Logger::Get().Win32Error("创建 third_party 失败");
		_Status(OnnxRuntimeStatus::Error);
		co_return;
	}

	_Status(OnnxRuntimeStatus::Downloading);

	// 下载留在 UI 线程：WinRT 的异步等待会回到同一个上下文，而状态事件最终
	// 会触碰 XAML，从线程池线程触发会抛 "marshalled for a different thread"。
	// 只有阻塞的解压切到后台，并且在报告状态前切回来。
	//
	// The download stays on the UI thread: the awaits below resume on the same
	// context, and the status events end up touching XAML, so firing them from
	// a thread-pool thread throws. Only the blocking extract goes to the
	// background, and every exit path returns here before reporting.
	bool ok = false;
	bool cancelled = false;

	try {
		HttpClient httpClient;
		auto requestProgressOp = httpClient.GetInputStreamAsync(Uri(RUNTIME_URL));

		uint64_t totalBytes = 0;
		requestProgressOp.Progress([&totalBytes](const auto&, const HttpProgress& progress) {
			if (std::optional<uint64_t> totalBytesToReceive = progress.TotalBytesToReceive) {
				totalBytes = *totalBytesToReceive;
			}
		});

		IInputStream httpStream = co_await requestProgressOp;

		bool downloaded = false;
		{
			wil::unique_hfile file(
				CreateFile2(archivePath.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr));
			if (!file) {
				Logger::Get().Win32Error("创建下载文件失败");
			} else {
				Buffer buffer(64 * 1024);
				// 这个包接近 1 GB，必须用 64 位计数
				// The package is close to 1 GB, so the counter has to be 64-bit.
				uint64_t bytesReceived = 0;
				bool failed = false;

				while (true) {
					IBuffer resultBuffer = co_await httpStream.ReadAsync(
						buffer, buffer.Capacity(), InputStreamOptions::Partial);

					if (_cancelled) {
						httpStream.Close();
						cancelled = true;
						break;
					}

					const uint32_t bufferSize = resultBuffer.Length();
					if (bufferSize == 0) {
						break;
					}

					if (!WriteFile(file.get(), resultBuffer.data(), bufferSize, nullptr, nullptr)) {
						Logger::Get().Win32Error("WriteFile 失败");
						failed = true;
						break;
					}

					bytesReceived += bufferSize;
					if (totalBytes > 0) {
						_downloadProgress = bytesReceived / (double)totalBytes;
						DownloadProgressChanged.Invoke(_downloadProgress);
					}
				}

				downloaded = !failed && !cancelled;
			}
		}

		if (cancelled) {
			DeleteFile(archivePath.c_str());
		} else if (downloaded) {
			_Status(OnnxRuntimeStatus::Extracting);

			// 解压会阻塞（WaitForSingleObject），必须切到后台
			// Extraction blocks on WaitForSingleObject, so it leaves the UI
			// thread here and returns below before anything is reported.
			co_await resume_background();

			if (_Extract(archivePath, thirdPartyDir)) {
				// 解压出来的可能是一层子目录，onnxruntime.dll 必须落在 third_party\ 下
				// The archive may unpack into a subdirectory; onnxruntime.dll has
				// to end up directly in third_party\ or PinThirdPartyRuntimes
				// will not find it.
				if (!IsInstalled()) {
					std::error_code ec;
					for (const auto& entry :
						std::filesystem::directory_iterator(thirdPartyDir, ec)) {
						if (!entry.is_directory(ec)) {
							continue;
						}

						const std::wstring inner = entry.path().wstring() + L"\\onnxruntime.dll";
						if (!Win32Helper::FileExists(inner.c_str())) {
							continue;
						}

						for (const auto& f :
							std::filesystem::directory_iterator(entry.path(), ec)) {
							std::filesystem::rename(f.path(),
								std::filesystem::path(thirdPartyDir) / f.path().filename(), ec);
						}
						std::filesystem::remove_all(entry.path(), ec);
						break;
					}
				}

				ok = IsInstalled();
				if (!ok) {
					Logger::Get().Error("解压后仍找不到 onnxruntime.dll");
				}
			}

			DeleteFile(archivePath.c_str());
		}
	} catch (const hresult_error& e) {
		Logger::Get().Error(StrHelper::Concat(
			"下载运行时失败 / failed to download the runtime: ",
			StrHelper::UTF16ToUTF8(e.message())));
		DeleteFile(archivePath.c_str());
	}

	// 可能仍在后台线程上（解压途中失败或抛异常），报告状态前必须切回
	// May still be on a background thread if the extract failed or threw, so
	// come back before touching anything the UI is bound to. Harmless when we
	// are already on the UI thread.
	co_await App::Get().Dispatcher();

	if (cancelled) {
		_downloadProgress = 0;
		_Status(OnnxRuntimeStatus::NotInstalled);
	} else {
		_Status(ok ? OnnxRuntimeStatus::Installed : OnnxRuntimeStatus::Error);
	}
}

}
