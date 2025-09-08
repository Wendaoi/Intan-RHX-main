#include "controlpanelgametab.h"
#include "controllerinterface.h"
#include "Engine/Threads/ponggame.h" // For ExperimentCondition enum
#include "Engine/Threads/gamethread.h" // For GameState struct

ControlPanelGameTab::ControlPanelGameTab(ControllerInterface* controllerInterface_, SystemState* state_, CommandParser* /*parser_*/, QWidget *parent) :
    QWidget(parent),
    state(state_),
    controllerInterface(controllerInterface_)
{
    // Game Control Group
    QGroupBox *controlGroupBox = new QGroupBox(tr("Game Control"));
    
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
    controlLayout->addRow(enableGameCheckBox);
    controlLayout->addRow(tr("Threshold Multiplier:"), thresholdMultiplierSpinBox);
    controlLayout->addRow(tr("Min Threshold:"), minThresholdSpinBox);
    controlLayout->addRow(tr("Refractory Period:"), refractoryPeriodSpinBox);
    controlLayout->addRow(tr("Experiment Condition:"), conditionComboBox);
    controlGroupBox->setLayout(controlLayout);

    // 游戏图形显示
    pongGameWidget = new PongGameWidget();
    pongGameWidget->setGameSize(640, 480);

    // Game State Group
    QGroupBox *stateGroupBox = new QGroupBox(tr("Game State"));
    ballPositionLabel = new QLabel(tr("Ball: (---, ---)"));
    paddlePositionLabel = new QLabel(tr("Paddle: ---"));
    bouncesLabel = new QLabel(tr("Bounces: 0"));

    QVBoxLayout *stateLayout = new QVBoxLayout;
    stateLayout->addWidget(ballPositionLabel);
    stateLayout->addWidget(paddlePositionLabel);
    stateLayout->addWidget(bouncesLabel);
    stateGroupBox->setLayout(stateLayout);

    // Main Layout
    QVBoxLayout *mainLayout = new QVBoxLayout;
    mainLayout->addWidget(controlGroupBox);
    mainLayout->addWidget(pongGameWidget);
    mainLayout->addWidget(stateGroupBox);
    mainLayout->addStretch(1);
    setLayout(mainLayout);
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

    if (!isRunning) {
        if (enableGameCheckBox->isChecked()) {
            enableGameCheckBox->setChecked(false); // This will trigger toggleGame(false)
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
