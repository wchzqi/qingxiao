// ============================================================================
// 青筱（QingXiaoType）P2-C：VoiceInputService 实现
// 对应头文件 VoiceInputService.h。
//
// 【本机说明】本文件在 Linux 环境仅做静态自检，不编译、不链接 sherpa-onnx。
//   Windows WASAPI 与 sherpa-onnx 真实调用点以注释标注，用 stub 实现占位，
//   保证头文件接口与整体流程框架可审、可贴入 VS2022 工程。
// ============================================================================

#include "stdafx.h"

#include "VoiceInputService.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

// ===== sherpa-onnx 头文件（Windows 侧真实 include；Linux 静态自检时注释掉） =====
// Windows 侧（VS2022，sherpa-onnx 预编译库 include 目录已加入附加包含目录）：
//   #include <sherpa-onnx/csrc/online-recognizer.h>
//   #include <sherpa-onnx/csrc/offline-recognizer.h>
//   #include <sherpa-onnx/csrc/vad-model.h>
//   #include <sherpa-onnx/csrc/silero-vad-model-config.h>
//
// 本机 Linux 静态自检：以下 stub 类型占位，保证符号可核对。
#ifdef WEASEL_WITH_SHERPA
  #include <sherpa-onnx/csrc/offline-recognizer.h>
  #include <sherpa-onnx/csrc/vad-model.h>
#else
  // Linux 自检占位：空类型，仅为编译通过。
  namespace sherpa_onnx {
  class OfflineRecognizer {};
  class VoiceActivityDetector {};
  }  // namespace sherpa_onnx
#endif

namespace weasel {

// ============================================================================
// EngineImpl：pimpl 封装 sherpa-onnx 句柄（头文件不暴露 onnxruntime）
// ============================================================================
struct VoiceInputService::EngineImpl {
  std::unique_ptr<sherpa_onnx::OfflineRecognizer> recognizer;
  std::unique_ptr<sherpa_onnx::VoiceActivityDetector> vad;
  bool initialized{false};
};

// ============================================================================
// 构造 / 析构
// ============================================================================
VoiceInputService::VoiceInputService() = default;

VoiceInputService::~VoiceInputService() {
  try {
    Stop();
  } catch (...) {
  }
  if (ready_event_) {
    CloseHandle(ready_event_);
    ready_event_ = NULL;
  }
}

// ============================================================================
// Configure：从 weasel.yaml voice/ 段读入配置后由 Handler 调用。
// ============================================================================
void VoiceInputService::Configure(const std::wstring& model_dir,
                                  bool vad_enabled,
                                  const std::wstring& hotkey_hint) {
  std::lock_guard<std::mutex> lock(mtx_);
  model_dir_ = model_dir;
  vad_enabled_ = vad_enabled;
  hotkey_hint_ = hotkey_hint;
  model_ready_ = _CheckModelFiles();
}

// ============================================================================
// _CheckModelFiles：检查模型目录关键文件是否齐备。
//   需要的文件（sherpa-onnx 中文 zipformer/paraformer 标准布局）：
//     model_dir/model.onnx          —— ASR 模型权重
//     model_dir/tokens.txt          —— token 表
//     model_dir/silero_vad.onnx     —— VAD 模型（vad_enabled=true 时必需）
// ============================================================================
bool VoiceInputService::_CheckModelFiles() {
  if (model_dir_.empty()) {
    return false;
  }
  std::error_code ec;
  namespace fs = std::filesystem;
  fs::path root(model_dir_);
  if (!fs::exists(root / L"model.onnx", ec)) return false;
  if (!fs::exists(root / L"tokens.txt", ec)) return false;
  if (vad_enabled_ && !fs::exists(root / L"silero_vad.onnx", ec)) return false;
  return true;
}

// ============================================================================
// Start：启动录音线程（非阻塞）。
// ============================================================================
bool VoiceInputService::Start(VoiceRecognizedCallback on_recognized) {
  if (thread_.joinable()) {
    return true;  // 已在录音
  }
  if (!model_ready_.load()) {
    state_ = VoiceState::ERROR_NOMODEL;
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mtx_);
    on_recognized_ = std::move(on_recognized);
  }

  stop_flag_ = false;
  if (!ready_event_) {
    ready_event_ = CreateEventW(NULL, TRUE, FALSE, NULL);
  }
  if (ready_event_) {
    ResetEvent(ready_event_);
  }

  try {
    thread_ = std::thread([this]() { _ThreadMain(); });
  } catch (...) {
    state_ = VoiceState::ERROR_FAIL;
    return false;
  }

  if (ready_event_) {
    WaitForSingleObject(ready_event_, 2000);
  }
  return state_.load() != VoiceState::ERROR_NOMIC;
}

// ============================================================================
// Stop：通知线程退出并 join。
// ============================================================================
void VoiceInputService::Stop() {
  stop_flag_ = true;
  if (thread_.joinable()) {
    try {
      thread_.join();
    } catch (...) {
    }
  }
  state_ = VoiceState::IDLE;
  _CloseMicrophone();
  _ReleaseEngine();
}

// ============================================================================
// _InitEngine：加载 sherpa-onnx 离线识别器 + VAD。
// Windows 侧真实 API 签名（来自 sherpa-onnx C++ wrapper）：
//
//   OfflineRecognizerConfig config;
//   config.model_config.zipformer2_transducer.model =
//       (model_dir_ + L"/model.onnx").string();
//   config.model_config.tokens =
//       (model_dir_ + L"/tokens.txt").string();
//   config.model_config.num_threads = 2;
//   config.model_config.provider = "cpu";
//   recognizer = std::make_unique<OfflineRecognizer>(config);
//
//   // VAD（silero-vad）
//   VadModelConfig vad_config;
//   vad_config.silero_vad.model = (model_dir_ + L"/silero_vad.onnx").string();
//   vad_config.silero_vad.threshold = 0.5f;
//   vad_config.silero_vad.min_silence_duration = 0.25f;  // 250ms 静音断句
//   vad_config.silero_vad.min_speech_duration = 0.25f;
//   vad_config.silero_vad.max_speech_duration = 20.0f;     // 最长 20s 强制断
//   vad = std::make_unique<VoiceActivityDetector>(vad_config, 30.0f /*buffer*/);
// ============================================================================
bool VoiceInputService::_InitEngine() {
  if (!model_ready_.load()) {
    return false;
  }
#ifdef _WIN32
  // Windows 侧：构造 sherpa_onnx::OfflineRecognizer + VoiceActivityDetector。
  // 具体参数见上方注释；此处为真实调用占位。
  engine_ = std::make_unique<EngineImpl>();
  // TODO(Windows): 填充 engine_->recognizer 与 engine_->vad。
  engine_->initialized = true;  // 自检占位
#else
  // Linux 静态自检：构造空 EngineImpl，不加载真实模型。
  engine_ = std::make_unique<EngineImpl>();
  engine_->initialized = true;  // 自检占位：视为初始化成功
#endif
  return engine_ && engine_->initialized;
}

void VoiceInputService::_ReleaseEngine() {
  if (engine_) {
    engine_->recognizer.reset();
    engine_->vad.reset();
    engine_->initialized = false;
  }
}

// ============================================================================
// _RecognizeBuffer：对一段完整 PCM（VAD 切出的语音段）跑离线识别。
// Windows 侧真实 API 签名：
//
//   auto stream = recognizer->CreateStream();
//   stream->AcceptWaveform(samples, num_samples);
//   recognizer->DecodeStream(stream.get());
//   std::string text = stream->GetResult().text;  // UTF-8
//   return text;
// ============================================================================
std::string VoiceInputService::_RecognizeBuffer(const float* samples,
                                                int num_samples) {
  if (!engine_ || !engine_->initialized || !samples || num_samples <= 0) {
    return std::string();
  }
#ifdef _WIN32
  // Windows 侧真实调用：
  //   auto stream = engine_->recognizer->CreateStream();
  //   stream->AcceptWaveform(16000.0f, samples, num_samples);
  //   engine_->recognizer->DecodeStream(stream.get());
  //   return stream->GetResult().text;
  return std::string("（Windows 侧真实识别结果占位）");
#else
  // Linux 自检：返回空串，不报错。
  return std::string();
#endif
}

// ============================================================================
// Windows WASAPI 录音采集（Windows 侧实现点）
//   采样率 16000 Hz / 单声道 / 16-bit PCM → 内部转 float32。
//   每帧 512 samples（= 32ms，匹配 silero-vad 推荐窗口）。
//   参考：Microsoft WASAPI Capture 示例
//   https://learn.microsoft.com/en-us/windows/win32/coreaudio/capturing-a-stream
// ============================================================================
bool VoiceInputService::_OpenMicrophone() {
#ifdef _WIN32
  // TODO(Windows): IMMDeviceEnumerator::GetDefaultAudioEndpoint(eCapture, eConsole)
  //   → IAudioClient::Initialize(AUDCLNT_SHAREMODE_SHARED, ...)
  //   → IAudioClient::GetService(IID_IAudioCaptureClient)
  //   → 格式协商：16kHz / mono / 16bit PCM。
  // 失败返回 false（state_ = ERROR_NOMIC）。
  return true;  // 自检占位
#else
  // Linux 自检：无 WASAPI，返回 false 表示无麦克风（不崩溃）。
  state_ = VoiceState::ERROR_NOMIC;
  return false;
#endif
}

void VoiceInputService::_CloseMicrophone() {
#ifdef _WIN32
  // TODO(Windows): Release IAudioCaptureClient / IAudioClient / IMMDevice。
#endif
}

bool VoiceInputService::_ReadFrame(float* samples, int num_samples) {
#ifdef _WIN32
  // TODO(Windows): IAudioCaptureClient::GetBuffer → 读 512 samples int16
  //   → 转 float32（/32768.0f）→ ReleaseBuffer。
  //   返回 false = 停止信号 / 设备断开。
  (void)samples; (void)num_samples;
  return !stop_flag_.load();  // 自检占位：stop_flag 触发即退出
#else
  (void)samples; (void)num_samples;
  return false;  // Linux 无音频硬件
#endif
}

// ============================================================================
// 录音线程主循环：
//   WASAPI 采集 → 16kHz float32 帧 → VAD AcceptWaveform
//     → VAD 检测到语音段结束 → 取完整段 → OfflineRecognizer 识别
//     → on_recognized_(text) 回调
// ============================================================================
void VoiceInputService::_ThreadMain() {
  state_ = VoiceState::RECORDING;

  // 1. 打开麦克风
  if (!_OpenMicrophone()) {
    state_ = VoiceState::ERROR_NOMIC;
    if (ready_event_) SetEvent(ready_event_);
    return;
  }

  // 2. 初始化识别引擎
  if (!_InitEngine()) {
    state_ = VoiceState::ERROR_NOMODEL;
    _CloseMicrophone();
    if (ready_event_) SetEvent(ready_event_);
    return;
  }

  if (ready_event_) {
    SetEvent(ready_event_);  // 通知 Start()：线程已就绪
  }

  // 3. 录音 + VAD 循环
  constexpr int kFrameSize = 512;  // silero-vad 推荐窗口（16kHz × 32ms）
  std::vector<float> frame(kFrameSize);
  std::vector<float> speech_segment;  // VAD 切出的语音段缓冲

  while (!stop_flag_.load()) {
    if (!_ReadFrame(frame.data(), kFrameSize)) {
      break;  // 麦克风断开 / 停止信号
    }

    if (vad_enabled_ && engine_ && engine_->vad) {
      // Windows 侧：VAD AcceptWaveform + IsDetected 轮询。
      //   engine_->vad->AcceptWaveform(16000.0f, frame.data(), kFrameSize);
      //   while (engine_->vad->IsDetected()) {
      //     auto segment = engine_->vad->Front();  // 取出一段语音
      //     std::string text = _RecognizeBuffer(segment.samples.data(),
      //                                          segment.samples.size());
      //     engine_->vad->Pop();
      //     if (!text.empty() && on_recognized_) {
      //       on_recognized_(text);  // 回调：服务端 commit 上屏
      //     }
      //   }
      // Linux 自检：空转，不做真实 VAD。
    } else {
      // 无 VAD 模式：手动停止时一次性识别整段。
      speech_segment.insert(speech_segment.end(), frame.begin(), frame.end());
    }
  }

  // 4. 退出前：若有 VAD 关闭时残留的语音段，Flush 后做最后一次识别。
  if (!vad_enabled_ && !speech_segment.empty()) {
    state_ = VoiceState::RECOGNIZING;
    std::string text = _RecognizeBuffer(speech_segment.data(),
                                        (int)speech_segment.size());
    if (!text.empty() && on_recognized_) {
      on_recognized_(text);
    }
  }

  // 5. 清理
  _CloseMicrophone();
  _ReleaseEngine();
  state_ = VoiceState::IDLE;
}

}  // namespace weasel
