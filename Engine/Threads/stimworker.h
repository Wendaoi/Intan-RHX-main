#ifndef STIMWORKER_H
#define STIMWORKER_H

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <QRandomGenerator>
#include <cmath>
#include <array>

// Lightweight worker that schedules stimulation events off the controller thread.
// It emits minimal trigger signals that can be executed on the controller thread
// to perform actual hardware calls (e.g., setManualStimTrigger).
class StimWorker : public QObject {
    Q_OBJECT
public:
    explicit StimWorker(QObject* parent = nullptr)
        : QObject(parent)
        , missTimer(nullptr)
    {
        lastSensoryTriggerMs.fill(0);
    }

    // Parameters (safe to tweak from owner thread via setters)
    void setSensoryRateRange(double minHz, double maxHz) { sensoryMinHz = minHz; sensoryMaxHz = maxHz; }
    void setGameWidth(double w) { gameWidth = w > 1.0 ? w : 640.0; }
    void setMinStimIntervalMs(int ms) { minStimIntervalMs = ms; }

signals:
    void triggerChannel(int zone);
    void triggerHitBurst();

public slots:
    // Must be called after moveToThread() so that QTimer is created in the worker thread.
    void init() {
        if (!missTimer) {
            missTimer = new QTimer(this);
            missTimer->setTimerType(Qt::PreciseTimer);
            connect(missTimer, &QTimer::timeout, this, &StimWorker::onMissTick);
        }
    }
    // Update state used for rate coding
    void updateGameState(int ballX) {
        lastBallX = ballX;
        haveGameState = true;
    }

    // Sensory: request a pulse for a zone; worker decides if interval passed.
    void requestSensory(int zone) {
        if (zone < 0 || zone > 7) return;
        if (silentActive && silentTimer.isValid() && silentTimer.elapsed() < silentDurationMs) return;
        else if (silentActive) silentActive = false;

        if (!sensoryClock.isValid()) sensoryClock.start();
        double bx = haveGameState ? (double)lastBallX : gameWidth;
        double proximity = 1.0 - (bx / gameWidth);
        if (proximity < 0.0) proximity = 0.0; if (proximity > 1.0) proximity = 1.0;
        double hz = sensoryMinHz + (sensoryMaxHz - sensoryMinHz) * proximity;
        if (hz < 0.1) hz = 0.1;
        int intervalMs = (int) std::lround(1000.0 / hz);

        qint64 nowMs = sensoryClock.elapsed();
        if (nowMs - lastSensoryTriggerMs[zone] >= intervalMs) {
            // Lightweight trace to help verify path
            // qDebug() << "[StimWorker] sensory emit zone" << zone << "interval" << intervalMs;
            emit triggerChannel(zone);
            lastSensoryTriggerMs[zone] = nowMs;
        }
    }

    // Hit: immediate burst (controller thread handles actual per-channel triggers)
    void requestHit() {
        if (silentActive && silentTimer.isValid() && silentTimer.elapsed() < silentDurationMs) return;
        else if (silentActive) silentActive = false;
        if (!hitThrottle.isValid()) hitThrottle.start();
        if (hitThrottle.elapsed() < minStimIntervalMs) return;
        emit triggerHitBurst();
        hitThrottle.restart();
    }

    // Miss: start 5 Hz, 4 s session of random single pulses
    void requestMiss() {
        if (silentActive && silentTimer.isValid() && silentTimer.elapsed() < silentDurationMs) return;
        else if (silentActive) silentActive = false;
        if (missActive) return;
        if (!missThrottle.isValid()) missThrottle.start();
        if (missThrottle.elapsed() < minStimIntervalMs) return;
        missTicksRemaining = 20; // 4 s * 5 Hz
        if (missTimer) missTimer->start(200);    // 200 ms interval
        missActive = true;
        missThrottle.restart();
    }

    // Silent feedback window (for Silent condition only)
    void startSilentWindow(int durationMs) {
        silentDurationMs = durationMs > 0 ? durationMs : silentDurationMs;
        silentActive = true;
        silentTimer.restart();
    }

private slots:
    void onMissTick() {
        if (!missActive) { if (missTimer) missTimer->stop(); return; }
        if (silentActive && silentTimer.isValid() && silentTimer.elapsed() < silentDurationMs) {
            // Skip this tick during silent window; still count down to keep window dominant
        } else {
            int zone = QRandomGenerator::global()->bounded(8);
            emit triggerChannel(zone);
        }
        if (--missTicksRemaining <= 0) {
            if (missTimer) missTimer->stop();
            missActive = false;
        }
    }

private:
    // State
    bool haveGameState = false;
    int lastBallX = 640;
    double gameWidth = 640.0;

    // Sensory gating
    QElapsedTimer sensoryClock;
    std::array<qint64, 8> lastSensoryTriggerMs;
    double sensoryMinHz = 4.0;
    double sensoryMaxHz = 40.0;

    // Feedback silent window
    bool silentActive = false;
    int silentDurationMs = 2000;
    QElapsedTimer silentTimer;

    // Miss session
    QTimer* missTimer;
    bool missActive = false;
    int missTicksRemaining = 0;

    // Throttles
    int minStimIntervalMs = 50;
    QElapsedTimer hitThrottle;
    QElapsedTimer missThrottle;
};

#endif // STIMWORKER_H
