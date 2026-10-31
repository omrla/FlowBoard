#pragma once

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// One ONNX model. It runs on the graphics card through ONNX Runtime + DirectML (any
// DirectX 12 GPU) when it can, and otherwise on the CPU through OpenCV DNN.
class NeuralNet {
public:
    NeuralNet();
    ~NeuralNet();
    NeuralNet(const NeuralNet&) = delete;
    NeuralNet& operator=(const NeuralNet&) = delete;

    // Loads the model, on the GPU first when `gpu` is true. Throws if it can't be loaded at all.
    void Load(const std::filesystem::path& path, bool gpu);

    // Runs one NHWC float input; returns the outputs in the model's order.
    std::vector<cv::Mat> Run(const cv::Mat& input);

    bool OnGpu() const { return m_gpu != nullptr; }
    // Why the GPU isn't used (empty when it is, or wasn't asked for).
    const std::string& GpuError() const { return m_gpuError; }

private:
    struct Gpu;   // the ONNX Runtime session; its headers stay out of the rest of the app
    std::unique_ptr<Gpu> m_gpu;
    cv::dnn::Net m_cpu;
    std::vector<cv::String> m_cpuOutputs;
    std::string m_gpuError;
};
