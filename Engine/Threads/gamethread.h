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
#include <map>
#include "waveformfifo.h"
#include "systemstate.h"
#include "filter.h"
#include "stimparameters.h"

// 尖峰检测结果结构
struct SpikeEvent {
    QString channelName;
    int64_t timestamp;
    float amplitude;
    float threshold;
};

// 通道处理结构
struct ChannelProcessor {
    QString name;
    SecondOrderHighpassFilter* highpassFilter;  // 2nd order Bessel high-pass filter (100Hz)
    FirstOrderLowpassFilter* lowpassFilter;     // 1st order Bessel low-pass filter (1Hz)
    float smoothedAbsValue;                     // 平滑后的绝对值
    float threshold;                            // 当前阈值
    float prevValue;                            // 上一个值，用于检测过零点
    int samplesSinceLastSpike;                  // 自上次尖峰以来的样本数
};

class GameThread : public QThread
{
    Q_OBJECT
public:
    explicit GameThread(WaveformFifo* waveformFifo_, SystemState* state_, QObject* parent = nullptr);
    ~GameThread();

    void run() override;
    void startRunning();
    void stopRunning();
    bool isActive() const;
    void close();

    // 游戏接口函数
    void setThresholdMultiplier(float multiplier);
    void setMinThreshold(float minThreshold);
    void setRefractoryPeriod(int samples);
    
    // 刺激控制接口
    void setStimChannelEnabled(const QString& channelName, bool enabled);
    void setStimChannelParameters(const QString& channelName, double frequency, double duration);
    void triggerStimChannel(const QString& channelName, double amplitude);

signals:
    // 尖峰检测信号
    void spikeDetected(const SpikeEvent& spike);
    void spikesPerSecondUpdated(const std::map<QString, float>& spikesPerSecond);
    
    // 状态信号
    void error(QString message);
    void statusUpdated(QString status);

public slots:
    void updateChannelList();

private:
    WaveformFifo* waveformFifo;
    SystemState* state;
    
    // 线程控制
    volatile bool keepGoing;
    volatile bool running;
    volatile bool stopThread;
    
    // 通道处理器
    std::vector<ChannelProcessor> channelProcessors;
    std::map<QString, int> channelIndexMap;
    
    // 尖峰检测参数
    float thresholdMultiplier;
    float minThreshold;
    int refractoryPeriod;
    
    // 统计数据
    std::map<QString, int> spikeCounters;
    std::map<QString, float> spikesPerSecond;
    int64_t lastStatsUpdate;
    
    // 初始化函数
    void initializeChannelProcessors();
    void cleanupChannelProcessors();
    
    // 核心处理函数
    void processSampleBlock(int numSamples);
    bool detectSpike(ChannelProcessor& processor, float value, int64_t timestamp);
    
    // 刺激控制函数
    void applyStimParameters(const QString& channelName);
    
    // 工具函数
    void updateSpikesPerSecond();
};

#endif // GAMETHREAD_H