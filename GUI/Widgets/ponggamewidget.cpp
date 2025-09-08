#include "ponggamewidget.h"
#include <QPainter>
#include <QPen>
#include <QBrush>

PongGameWidget::PongGameWidget(QWidget *parent) :
    QWidget(parent),
    gameWidth(640),
    gameHeight(480),
    paddleWidth(10),
    paddleHeight(320), // 默认值，将根据游戏状态动态更新
    ballSize(10)
{
    // 设置初始游戏状态，从右侧开始
    currentGameState = {80, 0, 630, 240, 320, 0, 0, 0.0f}; // paddle1Y, paddle2Y, ballX, ballY, paddleHeight, bounces, rallyCount, avgRallyLength

    // 设置固定尺寸（可以根据需要调整）
    setMinimumSize(320, 240);
    setMaximumSize(640, 480);
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

    // 计算缩放比例
    qreal scaleX = (qreal)width() / gameWidth;
    qreal scaleY = (qreal)height() / gameHeight;

    // 保存原始变换
    painter.save();
    painter.scale(scaleX, scaleY);

    // 设置画笔和刷子
    QPen pen(Qt::white);
    pen.setWidth(2);
    painter.setPen(pen);

    // 绘制游戏背景
    painter.fillRect(0, 0, gameWidth, gameHeight, Qt::black);

    // 绘制边框
    painter.drawRect(0, 0, gameWidth, gameHeight);

    // 移除中心线绘制


    // 绘制球拍1（玩家）
    painter.setBrush(Qt::white);
    painter.drawRect(0, state.paddle1Y, paddleWidth, state.paddleHeight);

    // 不绘制AI球拍（根据文档，球拍只能在左侧边缘）
    // 球会反弹，但是右侧没有球拍

    // 绘制小球
    painter.setBrush(Qt::white);
    painter.drawEllipse(state.ballX, state.ballY, ballSize, ballSize);

    // 恢复变换
    painter.restore();

    // 绘制游戏信息
    QPen textPen(Qt::black);
    painter.setPen(textPen);
    painter.setFont(QFont("Arial", 10));
    painter.drawText(10, 20, QString("Ball: (%1, %2)").arg(state.ballX).arg(state.ballY));
    painter.drawText(10, 35, QString("Paddle: %1").arg(state.paddle1Y));
}
