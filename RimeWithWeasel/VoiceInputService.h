// ============================================================================
// 青筱（QingXiaoType）P2-C：VoiceInputService 离线语音输入服务
//
// 归属工程：RimeWithWeasel/（与 ClipboardManager / NetworkGate 同工程，
//           由 RimeWithWeaselHandler 构造并持有）。
//
// 职责：
//   1) 热键触发后启动麦克风录音（WASAPI 采集，Windows 侧实现点标注）。
//   2) VAD（silero-vad，经 sherpa-onnx 封装）自动断句：检测到静音段即切分。
//   3) 离线 ASR（sherpa-onnx 中文 zipformer / paraformer）识别为文本。
//   4) 识别文本经 commit 链路直接上屏到焦点应用（复用 librime commit_composition
//      或 rime_api->commit_string 旁路，见 cpp 说明）。
//   5) 全程本地零上传——不发起任何网络请求，不过 NetworkGate。
//      （模型下载属安装期一次性联网，文档说明与 Gate 关系。）
//   6) 异常兜底：无麦克风 / 模型缺失 / 识别失败 → 返回 false + 提示文案，
//      绝不崩溃，不影响服务进程主链路。
//
// 线程模型（复用 P1 ClipboardManager 范式）：
//   - 独立 std::thread + message-only 隐藏窗承载录音循环与 VAD 推理。
//   - Start() 阻塞等待线程就绪事件（至多 2s）；Stop() 经 WM_CLOSE 退出并 join。
//   - 识别完成回调投递到服务端消息线程，经 IPC 响应行 / rime_api commit 上屏。
//
// 依赖：
//   - sherpa-onnx C++ API（OfflineRecognizer / VoiceActivityDetector）。
//   - Windows WASAPI（IMMDeviceEnumerator / IAudioCaptureClient）——Windows 侧。
//   - 本头文件不直接 include sherpa-onnx 头（避免污染 stdafx.h），
//     前置声明 + pimpl 方式持有引擎句柄。
// ============================================================================
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace weasel {

// 识别结果回调：text 为 UTF-8 字符串（识别出的中文文本）。
// 回调在录音/识别线程触发；实现方需自行 marshal 到服务端消息线程。
using VoiceRecognizedCallback = std::function<void(const std::string& text)>;

// 录音状态机
enum class VoiceState {
  IDLE = 0,       // 空闲（未录音）
  RECORDING,      // 录音中（VAD 监听）
  RECOGNIZING,    // 识别中（VAD 断句后提交 ASR）
  ERROR_NOMIC,    // 无麦克风 / 麦克风被占用
  ERROR_NOMODEL,  // 模型文件缺失
  ERROR_FAIL      // 识别失败 / 未知错误
};

class VoiceInputService {
 public:
  VoiceInputService();
  ~VoiceInputService();

  // 配置装载（由 RimeWithWeaselHandler::Initialize 在 config_open 后调用）。
  // model_dir: 模型目录绝对路径（如 %AppData%\Rime\models\sherpa-zh\）。
  // vad_enabled: 是否启用 VAD 自动断句（false = 手动按热键停止才识别）。
  // hotkey_hint: 仅日志用，实际热键由 TSF 侧配置解析。
  void Configure(const std::wstring& model_dir,
                 bool vad_enabled,
                 const std::wstring& hotkey_hint);

  // 启动录音 + VAD + 识别循环（非阻塞，内部起线程）。
  // 返回 false = 立即失败（无麦克风 / 模型缺失），调用方应提示用户。
  bool Start(VoiceRecognizedCallback on_recognized);

  // 停止录音并 join 线程（进程退出兜底）。
  void Stop();

  // 当前状态（供 voice.status 查询）。
  VoiceState GetState() const { return state_.load(); }

  // 是否已配置模型目录且模型文件存在（供 voice.status 回写）。
  bool IsModelReady() const { return model_ready_.load(); }

  // 是否正在录音（供 TSF 侧 UI 指示）。
  bool IsRecording() const {
    return state_.load() == VoiceState::RECORDING ||
           state_.load() == VoiceState::RECOGNIZING;
  }

 private:
  // 录音线程主循环（WASAPI 采集 → 16kHz 重采样 → VAD 喂入 → 断句送 ASR）。
  void _ThreadMain();

  // 检查模型目录与关键文件是否齐备（model.onnx / tokens.txt / silero_vad.onnx）。
  bool _CheckModelFiles();

  // ===== Windows WASAPI 录音采集（Windows 侧实现点） =====
  // 以下方法在非 Windows 平台为 stub（返回空缓冲），保证本机 Linux 静态编译可过。
  // Windows 侧实现：IMMDeviceEnumerator → IAudioClient → IAudioCaptureClient，
  // 16kHz / 单声道 / 16bit PCM，分帧 512 samples（匹配 silero-vad 窗口）。
  bool _OpenMicrophone();
  void _CloseMicrophone();
  // 读取一帧 PCM（16kHz mono float）；返回 false = 设备断开 / 停止信号。
  bool _ReadFrame(float* samples, int num_samples);

  // ===== sherpa-onnx 引擎封装（pimpl，避免头文件暴露 onnxruntime） =====
  // 前置声明：sherpa_onnx::OfflineRecognizer + sherpa_onnx::VoiceActivityDetector。
  struct EngineImpl;
  std::unique_ptr<EngineImpl> engine_;

  // 初始化 sherpa-onnx 识别器与 VAD；失败返回 false（state_ = ERROR_NOMODEL）。
  bool _InitEngine();
  void _ReleaseEngine();

  // 对一段 PCM 缓冲跑离线识别，返回 UTF-8 文本。
  std::string _RecognizeBuffer(const float* samples, int num_samples);

  // 状态原子量
  std::atomic<VoiceState> state_{VoiceState::IDLE};
  std::atomic<bool> model_ready_{false};
  std::atomic<bool> stop_flag_{false};

  // 配置（Configure 后只读，无需锁）
  std::wstring model_dir_;
  bool vad_enabled_{true};
  std::wstring hotkey_hint_;

  // 录音线程
  std::thread thread_;
  HANDLE ready_event_{NULL};   // 线程就绪事件（复用 ClipboardManager 范式）
  HWND hwnd_{NULL};            // message-only 窗口（WASAPI 不需要，但保留以统一消息循环范式）

  // 识别结果回调（Start 时注入）
  VoiceRecognizedCallback on_recognized_;

  // 互斥：保护 Start/Stop 与回调写入
  std::mutex mtx_;
};

}  // namespace weasel
