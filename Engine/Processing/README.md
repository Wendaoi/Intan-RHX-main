# Intan RHX 信号处理模块接口文档

## 1. 滤波器模块 (Filter)

### 1.1 基础滤波器类

#### Filter 类
基础滤波器接口类，定义了通用的滤波方法。

**主要方法:**
```cpp
void filter(const float* in, float* out, unsigned int length);
// 功能: 对输入数组进行滤波
// 参数:
//   in - 输入数据指针
//   out - 输出数据指针
//   length - 数据长度

void filter(const float* in, float* out, float* inMinusOut, unsigned int length);
// 功能: 滤波并返回差值(可用于高通滤波)
// 参数:
//   in - 输入数据指针
//   out - 滤波后输出数据指针
//   inMinusOut - 差值输出指针(in-out)
//   length - 数据长度

void filter(float* inout, unsigned int length);
// 功能: 原地滤波(覆盖输入数据)
// 参数:
//   inout - 输入/输出数据指针
//   length - 数据长度

virtual float filterOne(float in) = 0;
// 功能: 对单个样本进行滤波
// 参数:
//   in - 输入样本值
// 返回: 滤波后的样本值

virtual void reset() = 0;
// 功能: 重置滤波器状态
```

#### BiquadFilter 类
双二阶滤波器基类，继承自 Filter。

**成员变量:**
```cpp
float a1, a2, b0, b1, b2;  // 滤波器系数
bool isDcGainZero;         // DC增益是否为零
```

**主要方法:**
```cpp
float filterOne(float in) override;
// 功能: 使用直接形式I实现双二阶滤波器
// 参数:
//   in - 输入样本值
// 返回: 滤波后的样本值

void reset() override;
// 功能: 重置滤波器内部状态变量

// 系数获取方法:
float getA1() const;
float getA2() const;
float getB0() const;
float getB1() const;
float getB2() const;
```

### 1.2 具体滤波器实现

#### FirstOrderLowpassFilter 类
一阶低通滤波器。

**构造函数:**
```cpp
FirstOrderLowpassFilter(double fc, double sampleRate);
// 参数:
//   fc - 截止频率(Hz)
//   sampleRate - 采样率(Hz)
```

#### FirstOrderHighpassFilter 类
一阶高通滤波器。

**构造函数:**
```cpp
FirstOrderHighpassFilter(double fc, double sampleRate);
// 参数:
//   fc - 截止频率(Hz)
//   sampleRate - 采样率(Hz)
```

#### SecondOrderLowpassFilter 类
二阶低通滤波器。

**构造函数:**
```cpp
SecondOrderLowpassFilter(double fc, double q, double sampleRate);
// 参数:
//   fc - 截止频率(Hz)
//   q - 品质因子
//   sampleRate - 采样率(Hz)
```

#### SecondOrderHighpassFilter 类
二阶高通滤波器。

**构造函数:**
```cpp
SecondOrderHighpassFilter(double fc, double q, double sampleRate);
// 参数:
//   fc - 截止频率(Hz)
//   q - 品质因子
//   sampleRate - 采样率(Hz)
```

#### SecondOrderNotchFilter 类
二阶陷波滤波器。

**构造函数:**
```cpp
SecondOrderNotchFilter(double fNotch, double bandwidth, double sampleRate);
// 参数:
//   fNotch - 陷波频率(Hz)
//   bandwidth - 带宽(Hz)
//   sampleRate - 采样率(Hz)
```

## 2. 快速傅里叶变换模块 (FastFourierTransform)

### FFTCalculator 类
FFT计算器基类。

**主要方法:**
```cpp
virtual void reset(int numPoints) = 0;
// 功能: 重置FFT计算器
// 参数:
//   numPoints - FFT点数(必须是2的幂)

virtual void fft(float* x, float* y) = 0;
// 功能: 执行FFT变换
// 参数:
//   x - 实部输入/输出数组
//   y - 虚部输入/输出数组

virtual int getNumPoints() const = 0;
// 功能: 获取FFT点数
```

### 实现类
- `FFTRealCPU` - CPU实现的实数FFT
- `FFTComplexCPU` - CPU实现的复数FFT
- `FFTRealGPU` - GPU实现的实数FFT
- `FFTComplexGPU` - GPU实现的复数FFT

## 3. 软件参考处理器 (SoftwareReferenceProcessor)

### SoftwareReferenceProcessor 类
软件参考信号处理器。

**构造函数:**
```cpp
SoftwareReferenceProcessor(WaveformFifo* waveformFifo_, SystemState* state_);
// 参数:
//   waveformFifo_ - 波形FIFO指针
//   state_ - 系统状态指针
```

**主要方法:**
```cpp
void addOffChipChannel(const QString& channelName);
// 功能: 添加片外通道到参考列表
// 参数:
//   channelName - 通道名称

void removeOffChipChannel(const QString& channelName);
// 功能: 从参考列表中移除片外通道
// 参数:
//   channelName - 通道名称

void updateReferenceChannels();
// 功能: 更新参考通道列表

void processReferenceSignals(uint16_t* rhdData, uint16_t* referenceData,
                           vector<float> &dcAmplifierData, float* stimulationData,
                           int numSamples, int numDataStreams);
// 功能: 处理参考信号
// 参数:
//   rhdData - RHD数据指针
//   referenceData - 参考数据输出指针
//   dcAmplifierData - DC放大器数据向量
//   stimulationData - 刺激数据指针
//   numSamples - 样本数
//   numDataStreams - 数据流数
```

## 4. 波形FIFO (WaveformFifo)

### WaveformFifo 类
波形数据先进先出缓冲区。

**主要方法:**
```cpp
bool allocateChannels(WaveformFifoState state);
// 功能: 分配通道缓冲区
// 参数:
//   state - FIFO状态
// 返回: 是否成功

void freeChannels(WaveformFifoState state);
// 功能: 释放通道缓冲区
// 参数:
//   state - FIFO状态

float* getDataChannel(const QString& waveName);
// 功能: 获取数据通道指针
// 参数:
//   waveName - 波形名称
// 返回: 数据通道指针

void advanceReadPointer(WaveformFifoState state, int numWords);
// 功能: 移动读指针
// 参数:
//   state - FIFO状态
//   numWords - 移动的字数

int numWordsInMemory(WaveformFifoState state) const;
// 功能: 获取内存中的字数
// 参数:
//   state - FIFO状态
// 返回: 字数
```

## 5. 数据流FIFO (DataStreamFifo)

### DataStreamFifo 类
数据流先进先出缓冲区。

**主要方法:**
```cpp
bool allocateDataStream(const QString& streamName, int streamID, int fifoCapacity);
// 功能: 分配数据流
// 参数:
//   streamName - 流名称
//   streamID - 流ID
//   fifoCapacity - FIFO容量
// 返回: 是否成功

void freeDataStream(const QString& streamName);
// 功能: 释放数据流
// 参数:
//   streamName - 流名称

bool addData(const QString& streamName, const uint8_t* data, int dataSize);
// 功能: 添加数据到流
// 参数:
//   streamName - 流名称
//   data - 数据指针
//   dataSize - 数据大小
// 返回: 是否成功

int readData(const QString& streamName, uint8_t* buffer, int maxSize);
// 功能: 从流中读取数据
// 参数:
//   streamName - 流名称
//   buffer - 缓冲区指针
//   maxSize - 最大读取大小
// 返回: 实际读取的字节数
```

## 6. 阻抗读取器 (ImpedanceReader)

### ImpedanceReader 类
阻抗测量读取器。

**主要方法:**
```cpp
void measureImpedances();
// 功能: 执行阻抗测量

void calculateImpedanceValues();
// 功能: 计算阻抗值

double getMagnitude(int stream, int channel) const;
// 功能: 获取阻抗幅值
// 参数:
//   stream - 数据流索引
//   channel - 通道索引
// 返回: 阻抗幅值(欧姆)

double getPhase(int stream, int channel) const;
// 功能: 获取阻抗相位
// 参数:
//   stream - 数据流索引
//   channel - 通道索引
// 返回: 阻抗相位(度)
```

## 7. XPU接口模块

### AbstractXPUInterface 类
XPU接口抽象基类。

**主要方法:**
```cpp
virtual bool initialize() = 0;
// 功能: 初始化XPU接口
// 返回: 是否成功

virtual void cleanup() = 0;
// 功能: 清理XPU接口资源

virtual bool isAvailable() const = 0;
// 功能: 检查XPU是否可用
// 返回: 是否可用

virtual QString getName() const = 0;
// 功能: 获取接口名称
// 返回: 接口名称
```

### CPUInterface 类
CPU处理接口实现。

### GPUInterface 类
GPU处理接口实现。

## 使用示例

### 滤波器使用示例:
```cpp
// 创建低通滤波器
FirstOrderLowpassFilter lpFilter(300.0, 30000.0); // 300Hz截止频率，30kHz采样率

// 对数据进行滤波
vector<float> inputData = {...}; // 输入数据
vector<float> outputData(inputData.size());
lpFilter.filter(inputData.data(), outputData.data(), inputData.size());

// 重置滤波器状态
lpFilter.reset();
```

### FFT使用示例:
```cpp
// 创建FFT计算器
FFTRealCPU fftCalculator;
fftCalculator.reset(1024); // 1024点FFT

// 准备输入数据
vector<float> realData(1024, 0.0f);
vector<float> imagData(1024, 0.0f);

// 执行FFT
fftCalculator.fft(realData.data(), imagData.data());
```

### 参考信号处理示例:
```cpp
// 创建参考处理器
SoftwareReferenceProcessor refProcessor(waveformFifo, systemState);

// 添加参考通道
refProcessor.addOffChipChannel("ANALOG-IN-01");

// 更新参考通道
refProcessor.updateReferenceChannels();
```