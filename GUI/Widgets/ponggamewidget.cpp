#include "ponggamewidget.h"
#include <QPainter>
#include <QPen>
#include <QBrush>

PongGameWidget::PongGameWidget(QWidget *parent) :
    QWidget(parent),
    gameWidth(640),
    gameHeight(480),
    paddleWidth(10),
    paddleHeight(60), // 默认值，将根据游戏状态动态更新
    ballSize(10)
{
    // 设置初始游戏状态，从右侧开始
    currentGameState = {80, 0, 630, 240, 60, 0, 0, 0.0f}; // paddle1Y, paddle2Y, ballX, ballY, paddleHeight, bounces, rallyCount, avgRallyLength

    // 设置固定尺寸（可以根据需要调整）
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

PongGameWidget::~PongGameWidget()
{

}

void PongGameWidget::setGameSize(int width, int height)
{
    gameWidth = width;
    gameHeight = height;
    update(); // 重绘
}

void PongGameWidget::updateGameState(const GameState& gameState)
{
    QMutexLocker locker(&gameStateMutex);
    currentGameState.paddle1Y = gameState.paddle1Y;
    currentGameState.paddle2Y = gameState.paddle2Y;
    currentGameState.ballX = gameState.ballX;
    currentGameState.ballY = gameState.ballY;
    currentGameState.paddleHeight = gameState.paddleHeight; // 更新球拍高度
    update(); // 触发重绘
}

void PongGameWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // 获取当前游戏状态
    gameStateMutex.lock();
    GameState state = currentGameState;
    gameStateMutex.unlock();

    // 1. 计算等比缩放比例
    qreal scaleX = (qreal)width() / gameWidth;
    qreal scaleY = (qreal)height() / gameHeight;
    qreal scale = qMin(scaleX, scaleY);

    // 2. 计算缩放后的视口尺寸和偏移量以居中
    int viewportWidth = gameWidth * scale;
    int viewportHeight = gameHeight * scale;
    int offsetX = (width() - viewportWidth) / 2;
    int offsetY = (height() - viewportHeight) / 2;

    // 保存原始变换
    painter.save();

    // 3. 应用变换：先平移到居中位置，再缩放
    painter.translate(offsetX, offsetY);
    painter.scale(scale, scale);

    // --- 从这里开始，所有的绘制都将在一个 640x480 的逻辑坐标系中进行 ---

    // 设置画笔和刷子
    QPen pen(Qt::white);
    pen.setWidth(2);
    painter.setPen(pen);

    // 绘制游戏背景
    painter.fillRect(0, 0, gameWidth, gameHeight, Qt::black);

    // 绘制边框
    painter.drawRect(0, 0, gameWidth, gameHeight);

    // 绘制球拍1（玩家）
    painter.setBrush(Qt::white);
    painter.drawRect(0, state.paddle1Y, paddleWidth, state.paddleHeight);

    // 绘制小球
    painter.setBrush(Qt::white);
    painter.drawEllipse(state.ballX, state.ballY, ballSize, ballSize);

    // --- 逻辑坐标系绘制结束 ---

    // 恢复变换，以便在原始窗口坐标系中绘制文本
    painter.restore();

    // 在左上角绘制游戏信息（使用原始窗口坐标）
    QPen textPen(Qt::white);
    painter.setPen(textPen);
    painter.setFont(QFont("Arial", 10));
    painter.drawText(10, 20, QString("Ball: (%1, %2)").arg(state.ballX).arg(state.ballY));
    painter.drawText(10, 35, QString("Paddle: %1").arg(state.paddle1Y));
}
