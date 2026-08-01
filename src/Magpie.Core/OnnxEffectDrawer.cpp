#include "pch.h"
#include "OnnxEffectDrawer.h"
#include "Logger.h"
#include "DirectMLInferenceBackend.h"
#include "TensorRTInferenceBackend.h"
#include "Win32Helper.h"
#include <rapidjson/document.h>
#include "StrHelper.h"
#include "ScalingWindow.h"
#include "ScalingOptions.h"

namespace Magpie::Core {

OnnxEffectDrawer::OnnxEffectDrawer() {}

OnnxEffectDrawer::~OnnxEffectDrawer() {}

static bool ReadJson(
	const rapidjson::Document& doc,
	std::string& modelPath,
	uint32_t& scale,
	std::string& backend
) noexcept {
	if (!doc.IsObject()) {
		Logger::Get().Error("根元素不是 Object");
		return false;
	}

	auto root = ((const rapidjson::Document&)doc).GetObj();

	{
		auto node = root.FindMember("path");
		if (node == root.MemberEnd() || !node->value.IsString()) {
			Logger::Get().Error("解析 path 失败");
			return false;
		}

		modelPath = node->value.GetString();
	}
	
	{
		auto node = root.FindMember("scale");
		if (node == root.MemberEnd() || !node->value.IsUint()) {
			Logger::Get().Error("解析 scale 失败");
			return false;
		}

		scale = node->value.GetUint();
	}

	{
		auto node = root.FindMember("backend");
		if (node == root.MemberEnd() || !node->value.IsString()) {
			Logger::Get().Error("解析 backend 失败");
			return false;
		}

		backend = node->value.GetString();
	}

	return true;
}

bool OnnxEffectDrawer::Initialize(
	DeviceResources& deviceResources,
	BackendDescriptorStore& descriptorStore,
	ID3D11Texture2D** inOutTexture
) noexcept {
	std::string modelPath;
	uint32_t scale = 1;
	std::string backend;

	// The profile wins. Magpie's default profile doubles as the global
	// setting and named profiles override it, so this gives both global and
	// per-game config. model.json stays as a fallback for existing setups.
	const ScalingOptions& onnxOptions = ScalingWindow::Get().Options();
	const bool fromProfile = !onnxOptions.onnxModel.empty();
	if (fromProfile) {
		modelPath = StrHelper::UTF16ToUTF8(onnxOptions.onnxModel);
		scale = onnxOptions.onnxScale;
		backend = onnxOptions.onnxBackend == 1 ? "tensorrt" : "directml";
	}

	const wchar_t* jsonPath = L"model.json";
	if (!fromProfile && !Win32Helper::FileExists(jsonPath)) {
		// Relative path -> resolved against the working directory, not the exe
		// folder. Launched with the wrong cwd this silently disables ONNX, so
		// name the directory we actually looked in.
		wchar_t cwd[MAX_PATH]{};
		GetCurrentDirectoryW(MAX_PATH, cwd);
		Logger::Get().Info(StrHelper::Concat(
			"model.json not found in ", StrHelper::UTF16ToUTF8(cwd),
			" - ONNX upscaling disabled for this scale"));
		return true;
	}
	
	std::string json;
	if (!fromProfile) {
		if (!Win32Helper::ReadTextFile(jsonPath, json)) {
			Logger::Get().Error("Win32Helper::ReadTextFile 失败");
			return false;
		}

		{
		rapidjson::Document doc;
		doc.ParseInsitu(json.data());
		if (doc.HasParseError()) {
			Logger::Get().Error(fmt::format(
				"解析 json 失败 / model.json is not valid JSON (error {} at offset {}). "
				"A UTF-8 BOM is the usual cause - save it as UTF-8 without BOM.",
				uint32_t(doc.GetParseError()), doc.GetErrorOffset()));
			return false;
		}
		
		if (!ReadJson(doc, modelPath, scale, backend)) {
			Logger::Get().Error("ReadJson 失败");
			return false;
		}
		}
	}
	
	StrHelper::ToLowerCase(backend);
	if (backend == "directml" || backend == "dml" || backend == "d") {
		_inferenceBackend = std::make_unique<DirectMLInferenceBackend>();
	} else if (backend == "tensorrt" || backend == "trt" || backend == "t") {
		_inferenceBackend = std::make_unique<TensorRTInferenceBackend>();
	} else {
		Logger::Get().Error(StrHelper::Concat(
			"未知 backend '", backend,
			"' - expected one of: directml|dml|d, tensorrt|trt|t"));
		return false;
	}

	Logger::Get().Info(fmt::format(
		"ONNX model: path='{}' scale=x{} backend={} (from {})",
		modelPath, scale, backend, fromProfile ? "profile" : "model.json"));

	std::wstring modelPathW = StrHelper::UTF8ToUTF16(modelPath);
	if (!_inferenceBackend->Initialize(modelPathW.c_str(), scale, deviceResources, descriptorStore, *inOutTexture, inOutTexture)) {
		return false;
	}

	return true;
}

void OnnxEffectDrawer::Draw(EffectsProfiler& /*profiler*/) const noexcept {
	if (_inferenceBackend) {
		_inferenceBackend->Evaluate();
	}
}

}
