//------------------------------------------------------------------------------
//
//  Intan Technologies RHX Data Acquisition Software
//  Spike Detection and Game Interface Module
//
//  Copyright (c) 2020-2025 Intan Technologies
//
//  This file is part of the Intan Technologies RHX Data Acquisition Software.
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published
//  by the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//------------------------------------------------------------------------------

#ifndef GAMETHREAD_H
#define GAMETHREAD_H

#include <QObject>
#include <QThread>
#include <atomic>
#include <vector>
#include <deque>
#include <optional>
#include <map>
#include <unordered_set>
#include <mutex>
#include <shared_mutex>
#include <cstdint>
#include <chrono>
#include "waveformfifo.h"
#include "systemstate.h"
#include "filter.h"
#include "stimparameters.h"
#include "ponggame.h" // 包含新的游戏类

// 游戏状态结构，用于UI更新
struct GameState {
    int paddle1Y, paddle2Y, ballX, ballY;
    int paddleHeight; // 球拍高度
    int bounces; // 当前回合中的反弹次数
    int rallyCount; // 总回合数
    float avgRallyLength; // 平均回合长度
};

// 尖峰检测结果结构
struct SpikeEvent {
    QString channelName;
    int64_t timestamp;
    float amplitude;
    float threshold;
};

// 通道处理结构
struct ChannelProcessor {
    enum class DataSource {
        AnalogDC,
        GpuHighpass,
        GpuWideband,
        Invalid
    };

    QString name;
    double sampleRate;                          // 采样率（Hz）
    DataSource source;                          // 数据来源类型
    float* analogWaveform;                      // 指向CPU DC波形数据
    GpuWaveformAddress gpuAddress;              // GPU波形地址（高通或宽带）
    bool hasSpkDigital;                         // 是否存在GPU尖峰数字波形
    uint16_t* spkWaveform;                      // 指向GPU尖峰数字波形
    uint16_t* stimFlagsWaveform;                // 指向刺激标志数字波形 (channel|STIM)
    bool belongsToUpRegion;                     // 是否属于上移区域
    bool belongsToDownRegion;                   // 是否属于下移区域
    SecondOrderHighpassFilter* highpassFilter;  // 2nd order Bessel high-pass filter (100Hz)
    FirstOrderLowpassFilter* lowpassFilter;     // 1st order Bessel low-pass filter (1Hz)
    float smoothedAbsValue;                     // 平滑后的绝对值
    float threshold;                            // 当前阈值
    float prevValue;                            // 上一个值，用于检测过零点
    int samplesSinceLastSpike;                  // 自上次尖峰以来的样本数
    int bucketSpikeCount;                       // 当前桶内累计尖峰数
    bool stimActive;                            // 当前样本是否处于刺激标志状态
};

class ControllerInterface; // Forward declaration

#include "Engine/Threads/abstractgamecontroller.h"
#include "Engine/Threads/qlearningagent.h"

class SpikeBasedController : public AbstractGameController {
public:
    SpikeBasedController(std::function<std::pair<int, int>()> spike_counts_func) : get_spike_counts(spike_counts_func) {}
    PaddleAction getAction(const GameStateInfo& ) override {
        auto counts = get_spike_counts();
        if (counts.first > counts.second) return PaddleAction::MoveUp;
        if (counts.second > counts.first) return PaddleAction::MoveDown;
        return PaddleAction::Stay;
    }
    void update(const GameStateInfo&, PaddleAction, float, const GameStateInfo&) override {}
private:
    std::function<std::pair<int, int>()> get_spike_counts;
};


class GameThread : public QThread
{
    Q_OBJECT
public:
    GameThread(WaveformFifo* waveformFifo_, SystemState* state_, ControllerInterface* controllerInterface_, QObject* parent = nullptr);
    ~GameThread();

    void run() override;
    void startRunning();
    void stopRunning();
    void close();
    bool isActive() const;

    void setThresholdMultiplier(float multiplier);
    void setMinThreshold(float minThresholdValue);
    void setRefractoryPeriod(int samples);
    void setExperimentCondition(ExperimentCondition condition);
    void setMotorRegions(const std::vector<QString>& upChannels, const std::vector<QString>& downChannels);
    void setSensoryRegionChannels(const std::vector<QString>& sensoryChannels);
    void setDefaultMotorRegionsForDemo();

    void setStimChannelEnabled(const QString& channelName, bool enabled);
    void setStimChannelParameters(const QString& channelName, double frequency, double duration);
    void triggerStimChannel(const QString& channelName, double amplitude);

    void updateChannelList();

    // Functions for setting stim parameters
    void setHitStimAmplitude(double amplitude);
    void setHitStimFrequency(double frequency);
    void setHitStimDuration(double duration);
    void setMissStimAmplitude(double amplitude);
    void setMissStimFrequency(double frequency);
    void setMissStimDuration(double duration);


signals:
    void gameDataUpdated(const GameState& newState);
    void spikeDetected(const SpikeEvent& spike);
    void spikesPerSecondUpdated(const std::map<QString, float>& spikesPerSecond);
    // 简化版速率（例如平均Hz），便于跨线程UI显示
    void spikeRateScalar(float rateHz);
    void statusUpdated(const QString& status);
    void error(const QString& errorMsg);
    void sendHitStim();
    void sendMissStim();
    void stopAllStim();
    void sendSensoryStim(int zone);
    void startSilentWindow(int durationMs);
    // 异步请求控制器调制尖峰（避免在游戏线程中直接调用硬件接口导致阻塞）
    void requestModulateSpikes(int action);
    // 观测到芯片数据流中的刺激标志
    void stimObserved(const QString& channelName, uint32_t timeStamp);

private:
    void initializeThreadSafety();
    void initializeChannelProcessors();
    void cleanupChannelProcessors();
    void processSampleBlock(int numSamples);
    void applyStimParameters(const QString& channelName);

    // Thread-safe helper functions
    void resetBucketState(double sampleRate);
    std::optional<std::pair<int, int>> consumeCompletedBucketCounts();
    GameState safeGetCurrentGameState() const;

    void updateSpikesPerSecond();

    // Stimulation trigger functions
    void triggerHitStimulus();
    void triggerMissStimulus();
    void triggerSilentStimulus();

    // New functions for learning mode
    void initializeGameController();
    GameStateInfo getCurrentGameStateInfo();
    float calculateReward(GameEvent event, const PongGame& game);

    ControllerInterface* controllerInterface;
    WaveformFifo* waveformFifo;
    SystemState* state;
    PongGame* pongGame;
    std::unique_ptr<AbstractGameController> gameController;

    std::vector<ChannelProcessor> channelProcessors;
    std::map<QString, int> channelIndexMap;
    std::unordered_set<QString> motorUpSet;
    std::unordered_set<QString> motorDownSet;

    // Spike detection parameters
    std::mutex spikeDetectionMutex;
    float thresholdMultiplier;
    float minThreshold;
    int refractoryPeriod;

    // Motor region channels
    std::shared_mutex motorChannelsMutex;
    std::vector<QString> motorRegion1Channels; // Up
    std::vector<QString> motorRegion2Channels; // Down
    std::vector<QString> sensoryRegionChannels; // Sensory / stimulus region

    // Statistics
    mutable std::mutex statsMutex;
    int samplesPerBucket;
    int bucketSamplesRemaining;
    std::deque<std::pair<int, int>> completedBucketDiffs;
    // Sliding window（基于桶）的区域判定：默认500ms窗口（以桶为单位）
    std::deque<std::pair<int, int>> decisionWindow; // 近N个桶的(up, down)
    int bucketsPerDecisionWindow;                   // 决策窗口包含的桶数
    int windowSumUp;                                // 决策窗口内上区域尖峰和
    int windowSumDown;                              // 决策窗口内下区域尖峰和
    int latestBucketSpikeCountUp;
    int latestBucketSpikeCountDown;
    std::map<QString, int> spikeCounters;
    std::map<QString, float> spikesPerSecond;
    int64_t lastStatsUpdate;
    int totalRallyCount;
    int totalRallyLengthSum;
    float averageRallyLength;
    int lastSpikeDiff;
    int spikeDiffLogCounter;

    std::chrono::steady_clock::time_point lastGameStateEmit;
    std::chrono::milliseconds gameStateUiInterval;

    // Stimulation parameters
    std::mutex stimParamsMutex;
    double hitStimAmplitude, hitStimFrequency, hitStimDuration;
    double missStimAmplitude, missStimFrequency, missStimDuration;

    std::mutex signalSourcesMutex;

    std::atomic<bool> keepGoing;
    std::atomic<bool> running;
    std::atomic<bool> stopThread;
};

#endif // GAMETHREAD_H
