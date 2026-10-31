#include "NeuralNet.h"

#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>

namespace {

// One ONNX Runtime environment for the whole app; it must outlive every session.
Ort::Env& Environment()
{
    static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "FlowBoard");
    return env;
}

// UTF-8 text of a path; u8string() returns std::u8string from C++20 on.
std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto s = path.u8string();
    return std::string(s.begin(), s.end());
}

} // namespace

struct NeuralNet::Gpu {
    Ort::Session session{nullptr};
    std::string input;
    std::vector<std::string> outputs;       // in the order Run returns them
    std::vector<const char*> outputPtrs;
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
};

NeuralNet::NeuralNet() = default;
NeuralNet::~NeuralNet() = default;

void NeuralNet::Load(const std::filesystem::path& path, bool gpu)
{
    m_gpu.reset();
    m_gpuError.clear();
    if (gpu) {
        try {
            Ort::SessionOptions options;
            // DirectML needs these: no memory pattern planning, one node at a time.
            options.DisableMemPattern();
            options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            const OrtDmlApi* dml = nullptr;
            Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION,
                                                                     reinterpret_cast<const void**>(&dml)));
            Ort::ThrowOnError(dml->SessionOptionsAppendExecutionProvider_DML(options, 0));   // default adapter

            auto session = std::make_unique<Gpu>();
            session->session = Ort::Session(Environment(), path.c_str(), options);
            Ort::AllocatorWithDefaultOptions allocator;
            session->input = session->session.GetInputNameAllocated(0, allocator).get();
            for (size_t i = 0; i < session->session.GetOutputCount(); ++i)
                session->outputs.push_back(session->session.GetOutputNameAllocated(i, allocator).get());
            // Same order as OpenCV reports them (Identity, Identity_1, ...), which the
            // models' output parsing relies on.
            std::sort(session->outputs.begin(), session->outputs.end());
            for (const std::string& name : session->outputs)
                session->outputPtrs.push_back(name.c_str());
            m_gpu = std::move(session);
            return;
        } catch (const std::exception& e) {
            m_gpuError = e.what();
        }
    }

    // CPU: OpenCV DNN. Read the file ourselves so paths with non-ASCII characters work.
    std::ifstream file(path, std::ios::binary);
    if (!file)
        CV_Error(cv::Error::StsError, "cannot open model file " + PathToUtf8(path));
    const std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    m_cpu = cv::dnn::readNetFromONNX(data.data(), data.size());
    m_cpu.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    m_cpu.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
    m_cpuOutputs = m_cpu.getUnconnectedOutLayersNames();
}

std::vector<cv::Mat> NeuralNet::Run(const cv::Mat& input)
{
    std::vector<cv::Mat> outputs;
    if (!m_gpu) {
        m_cpu.setInput(input);
        m_cpu.forward(outputs, m_cpuOutputs);
        return outputs;
    }
    std::vector<int64_t> shape(input.size.p, input.size.p + input.dims);
    Ort::Value tensor = Ort::Value::CreateTensor<float>(m_gpu->memory, const_cast<float*>(input.ptr<float>()),
                                                        input.total(), shape.data(), shape.size());
    const char* in = m_gpu->input.c_str();
    std::vector<Ort::Value> results = m_gpu->session.Run(Ort::RunOptions{nullptr}, &in, &tensor, 1,
                                                         m_gpu->outputPtrs.data(), m_gpu->outputPtrs.size());
    for (Ort::Value& value : results) {
        std::vector<int> sizes;
        for (int64_t d : value.GetTensorTypeAndShapeInfo().GetShape())
            sizes.push_back(int(d));
        if (sizes.empty())
            sizes.push_back(1);
        outputs.push_back(cv::Mat(int(sizes.size()), sizes.data(), CV_32F, value.GetTensorMutableData<float>()).clone());
    }
    return outputs;
}
