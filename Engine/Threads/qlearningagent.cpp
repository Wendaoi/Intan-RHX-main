
#include "qlearningagent.h"
#include <algorithm>
#include <iostream>

QLearningAgent::QLearningAgent() : randomGenerator(std::random_device{}()) {
    qTable.clear();
}

// Convert the continuous state into a discrete, string-based representation
std::string QLearningAgent::discretizeState(const GameStateInfo& state) {
    // Discretize ball's Y position relative to the paddle
    int y_diff = static_cast<int>((state.ballY - state.paddleY) / 10.0f);
    // Discretize ball's X position
    int x_pos = static_cast<int>(state.ballX / 50.0f);
    // Discretize ball's Y velocity
    int vy = (state.ballVY > 0) ? 1 : -1;

    return std::to_string(y_diff) + "_" + std::to_string(x_pos) + "_" + std::to_string(vy);
}

PaddleAction QLearningAgent::getAction(const GameStateInfo& state) {
    std::string discreteState = discretizeState(state);

    // Initialize Q-values for new states
    if (qTable.find(discreteState) == qTable.end()) {
        qTable[discreteState] = {0.0f, 0.0f, 0.0f};
    }

    // Epsilon-greedy strategy for action selection
    std::uniform_real_distribution<> dist(0.0, 1.0);
    if (dist(randomGenerator) < epsilon) {
        // Explore: choose a random action
        std::uniform_int_distribution<> action_dist(0, 2);
        return static_cast<PaddleAction>(action_dist(randomGenerator));
    } else {
        // Exploit: choose the best known action
        const auto& q_values = qTable[discreteState];
        return static_cast<PaddleAction>(std::distance(q_values.begin(), std::max_element(q_values.begin(), q_values.end())));
    }
}

void QLearningAgent::update(const GameStateInfo& oldState, PaddleAction action, float reward, const GameStateInfo& newState) {
    std::string discreteOldState = discretizeState(oldState);
    std::string discreteNewState = discretizeState(newState);

    // Ensure Q-values for the new state are initialized
    if (qTable.find(discreteNewState) == qTable.end()) {
        qTable[discreteNewState] = {0.0f, 0.0f, 0.0f};
    }

    // Q-Learning update rule
    float old_q = qTable[discreteOldState][static_cast<int>(action)];
    float max_future_q = *std::max_element(qTable[discreteNewState].begin(), qTable[discreteNewState].end());
    float new_q = old_q + alpha * (reward + gamma * max_future_q - old_q);

    qTable[discreteOldState][static_cast<int>(action)] = new_q;

    // Decay epsilon to reduce exploration over time
    if (epsilon > minEpsilon) {
        epsilon *= epsilonDecay;
    }
}
