#include "controlpanelgametab.h"
#include "controllerinterface.h"
#include "Engine/Threads/ponggame.h" // For ExperimentCondition enum
#include "Engine/Threads/gamethread.h" // For GameState struct
#include <QTimer>
#include <QMessageBox>
#include <QStyle>

ControlPanelGameTab::ControlPanelGameTab(ControllerInterface* controllerInterface_, SystemState* state_, CommandParser* /*parser_*/, QWidget *parent) :
    QWidget(parent),
    state(state_),
    controllerInterface(controllerInterface_)
{
    // Game Control Group
    QGroupBox *controlGroupBox = new QGroupBox(tr("Game Control"));
    controlGroupBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
    controlGroupBox->setMinimumHeight(240);

    enableGameCheckBox = new QCheckBox(tr("Enable Pong Game"));
    connect(enableGameCheckBox, SIGNAL(toggled(bool)), this, SLOT(toggleGame(bool)));

    thresholdMultiplierSpinBox = new QDoubleSpinBox;
    thresholdMultiplierSpinBox->setRange(0.1, 10.0);
    thresholdMultiplierSpinBox->setSingleStep(0.1);
    thresholdMultiplierSpinBox->setValue(1.5); // Default value
    connect(thresholdMultiplierSpinBox, SIGNAL(valueChanged(double)), this, SLOT(setThresholdMultiplier(double)));

    minThresholdSpinBox = new QDoubleSpinBox;
    minThresholdSpinBox->setRange(0.0, 1000.0);
    minThresholdSpinBox->setSingleStep(1.0);
    minThresholdSpinBox->setValue(50.0); // Default value in uV
    minThresholdSpinBox->setSuffix(tr(" uV"));
    connect(minThresholdSpinBox, SIGNAL(valueChanged(double)), this, SLOT(setMinThreshold(double)));

    refractoryPeriodSpinBox = new QSpinBox;
    refractoryPeriodSpinBox->setRange(0, 1000);
    refractoryPeriodSpinBox->setSingleStep(10);
    refractoryPeriodSpinBox->setValue(100); // Default value in samples
    refractoryPeriodSpinBox->setSuffix(tr(" samples"));
    connect(refractoryPeriodSpinBox, SIGNAL(valueChanged(int)), this, SLOT(setRefractoryPeriod(int)));

    conditionComboBox = new QComboBox;
    conditionComboBox->addItem(tr("Stimulus"));
    conditionComboBox->addItem(tr("Silent"));
    conditionComboBox->addItem(tr("No-feedback"));
    conditionComboBox->addItem(tr("Rest"));
    connect(conditionComboBox, SIGNAL(currentIndexChanged(int)), this, SLOT(setExperimentCondition(int)));

    QFormLayout *controlLayout = new QFormLayout;
    controlLayout->setVerticalSpacing(5);
    controlLayout->addRow(enableGameCheckBox);
    controlLayout->addRow(tr("Threshold Multiplier:"), thresholdMultiplierSpinBox);
    controlLayout->addRow(tr("Min Threshold:"), minThresholdSpinBox);
    controlLayout->addRow(tr("Refractory Period:"), refractoryPeriodSpinBox);
    controlLayout->addRow(tr("Experiment Condition:"), conditionComboBox);

    // Current targets (µA)
    hitTargetCurrentSpinBox = new QDoubleSpinBox;
    hitTargetCurrentSpinBox->setRange(0.0, 2000.0);
    hitTargetCurrentSpinBox->setSingleStep(0.5);
    hitTargetCurrentSpinBox->setValue(1.0);
    hitTargetCurrentSpinBox->setSuffix(tr(" µA"));
    connect(hitTargetCurrentSpinBox, SIGNAL(valueChanged(double)), this, SLOT(setHitTargetCurrent(double)));

    missTargetCurrentSpinBox = new QDoubleSpinBox;
    missTargetCurrentSpinBox->setRange(0.0, 2000.0);
    missTargetCurrentSpinBox->setSingleStep(0.5);
    missTargetCurrentSpinBox->setValue(2.0);
    missTargetCurrentSpinBox->setSuffix(tr(" µA"));
    connect(missTargetCurrentSpinBox, SIGNAL(valueChanged(double)), this, SLOT(setMissTargetCurrent(double)));

    sensoryTargetCurrentSpinBox = new QDoubleSpinBox;
    sensoryTargetCurrentSpinBox->setRange(0.0, 2000.0);
    sensoryTargetCurrentSpinBox->setSingleStep(0.5);
    sensoryTargetCurrentSpinBox->setValue(1.0);
    sensoryTargetCurrentSpinBox->setSuffix(tr(" µA"));
    connect(sensoryTargetCurrentSpinBox, SIGNAL(valueChanged(double)), this, SLOT(setSensoryTargetCurrent(double)));

    missFreezeDurationSpinBox = new QSpinBox;
    missFreezeDurationSpinBox->setRange(0, 10000);
    missFreezeDurationSpinBox->setSingleStep(100);
    missFreezeDurationSpinBox->setValue(2000);
    missFreezeDurationSpinBox->setSuffix(tr(" ms"));
    connect(missFreezeDurationSpinBox, SIGNAL(valueChanged(int)), this, SLOT(setMissFreezeDuration(int)));

    controlLayout->addRow(tr("Hit Target Current:"), hitTargetCurrentSpinBox);
    controlLayout->addRow(tr("Miss Target Current:"), missTargetCurrentSpinBox);
    controlLayout->addRow(tr("Sensory Target Current:"), sensoryTargetCurrentSpinBox);
    controlLayout->addRow(tr("Miss Freeze Duration:"), missFreezeDurationSpinBox);
    controlGroupBox->setLayout(controlLayout);

    // 游戏图形显示
    pongGameWidget = new PongGameWidget();
    pongGameWidget->setGameSize(640, 480);

    // Game State Group
    QGroupBox *stateGroupBox = new QGroupBox(tr("Game State"));
    ballPositionLabel = new QLabel(tr("Ball: (---, ---)"));
    paddlePositionLabel = new QLabel(tr("Paddle: ---"));
    bouncesLabel = new QLabel(tr("Bounces: 0"));
    // 状态显示
    spikeRateLabel = new QLabel(tr("Spike Rate: N/A"));
    validationStatusLabel = new QLabel(tr("Parameters are valid."));




    QVBoxLayout *stateLayout = new QVBoxLayout;
    stateLayout->addWidget(ballPositionLabel);
    stateLayout->addWidget(paddlePositionLabel);
    stateLayout->addWidget(bouncesLabel);
    // 移除 CPU/FIFO 指标，仅保留 spike 速率与参数校验
    stateLayout->addWidget(spikeRateLabel);
    stateLayout->addWidget(validationStatusLabel);
    stateGroupBox->setLayout(stateLayout);

    // Main Layout
    QVBoxLayout *mainLayout = new QVBoxLayout;
    mainLayout->addWidget(controlGroupBox);
    mainLayout->addWidget(stateGroupBox);
    mainLayout->addStretch(1);
    setLayout(mainLayout);
}

PongGameWidget* ControlPanelGameTab::getPongGameWidget() const
{
    return pongGameWidget;
}

void ControlPanelGameTab::updateFromState()
{
    bool isRunning = state->running;
    bool gameEnabled = enableGameCheckBox->isChecked();

    // Enable game checkbox only when acquisition is running
    enableGameCheckBox->setEnabled(isRunning);
    
    // Enable game parameter controls only when acquisition is running AND game is enabled
    thresholdMultiplierSpinBox->setEnabled(isRunning && gameEnabled);
    minThresholdSpinBox->setEnabled(isRunning && gameEnabled);
    refractoryPeriodSpinBox->setEnabled(isRunning && gameEnabled);
    conditionComboBox->setEnabled(isRunning && gameEnabled);
    missFreezeDurationSpinBox->setEnabled(isRunning && gameEnabled);

    if (!isRunning) {
        if (enableGameCheckBox->isChecked()) {
            // enableGameCheckBox->setChecked(false); // This will trigger toggleGame(false)
        }
        updateGameData(GameState{0, 0, 0, 0, 320, 0, 0, 0.0f});
    }
}

void ControlPanelGameTab::updateGameData(const GameState& gameState)
{
    if (!enableGameCheckBox->isChecked()) {
        ballPositionLabel->setText(tr("Ball: (---, ---)"));
        paddlePositionLabel->setText(tr("Paddle: ---"));
        bouncesLabel->setText(tr("Bounces: 0"));
        return;
    }

    // 更新图形显示
    pongGameWidget->updateGameState(gameState);

    // 更新文本标签
    ballPositionLabel->setText(QString("Ball: (%1, %2)").arg(QString::number(gameState.ballX)).arg(QString::number(gameState.ballY)));
    paddlePositionLabel->setText(QString("Paddle: %1").arg(QString::number(gameState.paddle1Y)));
    bouncesLabel->setText("Bounces: " + QString::number(gameState.bounces) +
                          " (Rally #" + QString::number(gameState.rallyCount) +
                          ", Avg: " + QString::number(gameState.avgRallyLength, 'f', 1) + ")");
}

void ControlPanelGameTab::toggleGame(bool enabled)
{
    emit setGameEnabled(enabled);
    controllerInterface->toggleGameThread(enabled);
    updateFromState();
}

void ControlPanelGameTab::setThresholdMultiplier(double value)
{
    controllerInterface->setGameThresholdMultiplier(value);
}

void ControlPanelGameTab::setMinThreshold(double value)
{
    controllerInterface->setGameMinThreshold(value);
}

void ControlPanelGameTab::setRefractoryPeriod(int value)
{
    controllerInterface->setGameRefractoryPeriod(value);
}

void ControlPanelGameTab::setExperimentCondition(int index)
{
    // Map combo box index to ExperimentCondition enum
    ExperimentCondition condition;
    switch (index) {
        case 0: condition = ExperimentCondition::Stimulus; break;
        case 1: condition = ExperimentCondition::Silent; break;
        case 2: condition = ExperimentCondition::NoFeedback; break;
        case 3: condition = ExperimentCondition::Rest; break;
        default: condition = ExperimentCondition::Rest; break;
    }
    controllerInterface->setGameExperimentCondition(static_cast<int>(condition));
}

void ControlPanelGameTab::updateSpikeRate(const std::map<QString, float>& spikesPerSecond)
{
    // For simplicity, just display the spike rate of the first channel, or an average.
    // A more sophisticated display might involve a graph or list.
    if (!spikesPerSecond.empty()) {
        QString channelName = spikesPerSecond.begin()->first;
        float rate = spikesPerSecond.begin()->second;
        spikeRateLabel->setText(QString("Spike Rate (%1): %2 Hz").arg(channelName).arg(rate, 0, 'f', 1));
    } else {
        spikeRateLabel->setText("Spike Rate: N/A");
    }
}

void ControlPanelGameTab::updateSpikeRateScalar(float rateHz)
{
    spikeRateLabel->setText(QString("Spike Rate: %1 Hz").arg(rateHz, 0, 'f', 1));
}

// 移除 CPU/FIFO 指标更新函数

void ControlPanelGameTab::setHitStimAmplitude(double value)
{
    controllerInterface->setHitStimAmplitude(value);
}

void ControlPanelGameTab::setHitStimFrequency(double value)
{
    controllerInterface->setHitStimFrequency(value);
}

void ControlPanelGameTab::setHitStimDuration(double value)
{
    controllerInterface->setHitStimDuration(value);
}

void ControlPanelGameTab::setMissStimAmplitude(double value)
{
    controllerInterface->setMissStimAmplitude(value);
}

void ControlPanelGameTab::setMissStimFrequency(double value)
{
    controllerInterface->setMissStimFrequency(value);
}

void ControlPanelGameTab::setMissStimDuration(double value)
{
    controllerInterface->setMissStimDuration(value);
}

void ControlPanelGameTab::setHitTargetCurrent(double ua)
{
    controllerInterface->setHitTargetCurrent(ua);
}

void ControlPanelGameTab::setMissTargetCurrent(double ua)
{
    controllerInterface->setMissTargetCurrent(ua);
}

void ControlPanelGameTab::setSensoryTargetCurrent(double ua)
{
    controllerInterface->setSensoryTargetCurrent(ua);
}

void ControlPanelGameTab::setMissFreezeDuration(int value)
{
    controllerInterface->setMissFreezeDurationMs(value);
}

void ControlPanelGameTab::validateParameters()
{
    // Example validation: check if stimulus parameters are within safe ranges
    // This is a placeholder; actual validation logic would be more complex
    bool valid = true;
    QString message = "Parameters are valid.";

    if (hitStimAmplitudeSpinBox->value() > 2000.0 || missStimAmplitudeSpinBox->value() > 2000.0) {
        message = "Warning: Stimulus amplitude exceeds recommended limits.";
        valid = false;
    }

    if (hitStimDurationSpinBox->value() < 10.0 || missStimDurationSpinBox->value() < 10.0) {
        message = "Warning: Stimulus duration is too short.";
        valid = false;
    }

    if (valid) {
        validationStatusLabel->setStyleSheet("color: green");
    } else {
        validationStatusLabel->setStyleSheet("color: orange");
    }
    validationStatusLabel->setText(message);
}
