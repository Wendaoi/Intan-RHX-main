#include "ponggame.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

PongGame::PongGame() {
    gameWidth = 640;
    gameHeight = 480;
    paddleWidth = 0;
    paddleHeight = 320;
    ballSize = 0; // 球无体积，作为点处理
    currentCondition = ExperimentCondition::Stimulus;
    resetBall(true);
    bounces_in_rally = 0;
    paddleY = gameHeight / 2 - paddleHeight / 2;
}

void PongGame::resetBall(bool randomVector) {
    ballX = static_cast<float>(gameWidth);  // 球点从右侧边界出发
    ballY = static_cast<float>(gameHeight) / 2.0f;
    if (randomVector) {
        ballSpeedX = (rand() % 2 == 0) ? -8.0f : -6.0f;  // 向左运动
        ballSpeedY = static_cast<float>((rand() % 10) - 5);
        
    } else {
        ballSpeedX = -7.0f;  // 向左运动，固定速度
        ballSpeedY = 3.0f;   // 增加Y方向速度
    }
}

void PongGame::setCondition(ExperimentCondition condition) {
    currentCondition = condition;
}

ExperimentCondition PongGame::getCondition() const {
    return currentCondition;
}

GameEvent PongGame::update(int spikesUp, int spikesDown) {
    // 1. 根据尖峰更新玩家球拍位置
    int paddleSpeed = 5;
    if (spikesUp > spikesDown) {
        paddleY -= paddleSpeed;
    } else if (spikesDown > spikesUp) {
        paddleY += paddleSpeed;
    }
    paddleY = std::max(0, std::min(paddleY, gameHeight - paddleHeight));

    // 2. 使用增量的方式逐步更新球位置，处理碰撞
    GameEvent event = updateBallPosition();

    return event;
}

GameEvent PongGame::updateBallPosition() {
    // 球作为点处理，简化解算
    // 移动球点到新位置
    float newBallX = ballX + ballSpeedX;
    float newBallY = ballY + ballSpeedY;

    // 2.1 处理X轴碰撞检测（球拍和边界）
    if (ballSpeedX < 0.0f && checkPaddleCollision(newBallX, newBallY)) { // 仅当球向左移动检测球拍碰撞
        // 球拍碰撞，反弹（简化逻辑，与边界反弹一致）
        ballSpeedX = -ballSpeedX;

        // 更新位置（反弹后立即反向移动一步，以避免立即再次碰撞）
        ballX = - newBallX; 
        ballY = newBallY;

        return GameEvent::BallHitPlayerPaddle;
    } else {
        // 无球拍碰撞，检查左右边界
        if (newBallX <= 0.0f) {
            // 球点越过左侧边界（玩家未接住）
            ballX = 0.0f;
            ballSpeedX = -ballSpeedX; // 反弹，但实际是重置

            if (currentCondition != ExperimentCondition::NoFeedback) {
                bounces_in_rally = 0;
                resetBall(true);
            }
            return GameEvent::PlayerMissed;
        } else if (newBallX >= static_cast<float>(gameWidth)) {
            // 球点碰到右侧边界，反弹
            ballX = static_cast<float>(gameWidth);
            ballSpeedX = -ballSpeedX;
            // 不更新Y位置，因为未移动到新位置
        } else {
            // 正常移动
            ballX = newBallX;
        }
    }

    // 2.2 处理Y轴边界碰撞
    if (newBallY <= 0.0f) {
        // 球点碰到上边界，反弹
        ballY = 0.0f;
        ballSpeedY = -ballSpeedY;
    } else if (newBallY >= static_cast<float>(gameHeight)) {
        // 球点碰到下边界，反弹
        ballY = static_cast<float>(gameHeight);
        ballSpeedY = -ballSpeedY;
    } else {
        // 正常移动
        ballY = newBallY;
    }

    return GameEvent::None;
}

bool PongGame::checkPaddleCollision(float newBallX, float newBallY) {
    // 球拍视为垂直线段 x=0，从 y=paddleY 到 y=paddleY + paddleHeight
    // 球视为点，从 (ballX, ballY) 到 (newBallX, newBallY)
    // 仅当球向左移动且轨迹穿过 x=0 时检测

    if (!(newBallX <= 0.0f)) { // 球未从右侧越过x=0
        return false;
    }

    // 计算球轨迹与 x=0 的交点Y坐标（简化解算）
    float y_intersect = ballY + (0.0f - ballX) * (newBallY - ballY) / (newBallX - ballX);

    // 检查交点是否在球拍线段范围内
    if (y_intersect >= static_cast<float>(paddleY) && y_intersect <= static_cast<float>(paddleY + paddleHeight)) {
        bounces_in_rally++;
        return true;
    }

    return false;
}

int PongGame::getSensoryStimZone() const {
    if (currentCondition == ExperimentCondition::Rest) {
        return -1; // Rest条件下无感觉输入
    }

    // 将球点相对于球拍线段的位置映射到8个感觉刺激区域
    // 区域 0 = 最底部，区域 7 = 最顶部
    // 基于球点Y坐标到球拍中心的距离

    // 计算球点和球拍中心的Y坐标
    float ballCenterY = ballY; // 球为点，其位置即中心
    float paddleCenterY = static_cast<float>(paddleY + paddleHeight) / 2.0f;

    // 计算相对位置 (球相对于球拍中心的Y偏移)
    float relativeY = ballCenterY - paddleCenterY;

    // 将相对位置映射到8个区域 (范围 [-gameHeight/2, gameHeight/2] 均匀分为8个区域)
    const float zoneRange = static_cast<float>(gameHeight) / 8.0f;
    int offsetY = static_cast<int>((relativeY + gameHeight / 2.0f) / zoneRange);

    // 边界处理：确保在0-7范围内
    if (offsetY < 0) offsetY = 0;
    if (offsetY > 7) offsetY = 7;

    return offsetY;
}
