#include "pch.h"
#include "OnnxEffectDrawer.h"
#include "Logger.h"
#include "OnnxStatus.h"
#include "DirectMLInferenceBackend.h"
#include "TensorRTInferenceBackend.h"
#include "Win32Helper.h"
#include <rapidjson/document.h>
#include "StrHelper.h"
#include "ScalingWindow.h"
#include "ScalingOptions.h"
#include "OnnxHelper.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "BackendDescriptorStore.h"
#include "shaders/DownscaleCS.h"

namespace Magpie {

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
	// 预降采样：模型在更低分辨率上运行，然后由它放大回去
	// Pre-downscale: run the model at a lower resolution and let it scale back
	// up. Without this a native-resolution window has nothing to upscale.
	ID3D11Texture2D* backendInput = *inOutTexture;
	if (onnxOptions.onnxRenderWidth != 0 && onnxOptions.onnxRenderHeight != 0) {
		const SIZE srcSize = OnnxHelper::GetTextureSize(*inOutTexture);
		const uint32_t dstWidth = std::min((uint32_t)srcSize.cx, onnxOptions.onnxRenderWidth);
		const uint32_t dstHeight = std::min((uint32_t)srcSize.cy, onnxOptions.onnxRenderHeight);

		// 放大才有意义 / only worth doing when it actually reduces the size
		if (dstWidth < (uint32_t)srcSize.cx || dstHeight < (uint32_t)srcSize.cy) {
			_d3dDC = deviceResources.GetD3DDC();

			_downscaledTex = DirectXHelper::CreateTexture2D(
				deviceResources.GetD3DDevice(),
				DXGI_FORMAT_R8G8B8A8_UNORM,
				dstWidth,
				dstHeight,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
			);
			if (!_downscaledTex) {
				Logger::Get().Error("创建预降采样纹理失败 / failed to create the downscale texture");
				return false;
			}

			_srcSrv = descriptorStore.GetShaderResourceView(*inOutTexture);
			_downscaledUav = descriptorStore.GetUnorderedAccessView(_downscaledTex.get());
			// 线性采样 = 双线性缩小 / linear sampling gives a bilinear downscale
			_sampler = deviceResources.GetSampler(
				D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP);
			if (!_srcSrv || !_downscaledUav || !_sampler) {
				Logger::Get().Error("创建预降采样视图失败 / failed to create downscale views");
				return false;
			}

			HRESULT hr = deviceResources.GetD3DDevice()->CreateComputeShader(
				DownscaleCS, std::size(DownscaleCS), nullptr, _downscaleShader.put());
			if (FAILED(hr)) {
				Logger::Get().ComError("CreateComputeShader 失败", hr);
				return false;
			}

			_downscaleDispatch = { (dstWidth + 7) / 8, (dstHeight + 7) / 8 };
			backendInput = _downscaledTex.get();

			Logger::Get().Info(fmt::format(
				"ONNX pre-downscale: {}x{} -> {}x{} before inference",
				srcSize.cx, srcSize.cy, dstWidth, dstHeight));
		}
	}

	if (!_inferenceBackend->Initialize(modelPathW.c_str(), scale, deviceResources, descriptorStore, backendInput, inOutTexture)) {
		// 不要留下半初始化的后端，否则 Draw 会继续调用它
		// Do not leave a half-initialized backend behind - Draw would keep
		// calling into it.
		_inferenceBackend.reset();

		Logger::Get().Error(
			"初始化推理后端失败 / inference backend failed to initialize. Usual "
			"causes: the scale does not match the model, or the model is not a "
			"supported [-1,3,-1,-1] NCHW fp16/fp32 upscaler.");
		OnnxStatus::Report(L"AI upscaling failed",
			L"The model could not be initialized. Check that the scale matches the "
			L"model, then see logs\\magpie.log.");
		return false;
	}

	return true;
}

void OnnxEffectDrawer::Draw(EffectsProfiler& /*profiler*/) const noexcept {
	if (_downscaleShader) {
		_d3dDC->CSSetShader(_downscaleShader.get(), nullptr, 0);
		_d3dDC->CSSetShaderResources(0, 1, &_srcSrv);
		_d3dDC->CSSetSamplers(0, 1, &_sampler);
		_d3dDC->CSSetUnorderedAccessViews(0, 1, &_downscaledUav, nullptr);
		_d3dDC->Dispatch(_downscaleDispatch.first, _downscaleDispatch.second, 1);

		// 解绑 UAV，否则后续把它作为 SRV 绑定会失败
		// Unbind, or binding it as an SRV afterwards silently fails.
		ID3D11UnorderedAccessView* nullUav = nullptr;
		_d3dDC->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	}

	if (_inferenceBackend) {
		_inferenceBackend->Evaluate();
	}
}

}
