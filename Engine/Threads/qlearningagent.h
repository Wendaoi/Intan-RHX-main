
#ifndef QLEARNINGAGENT_H
#define QLEARNINGAGENT_H

#include "abstractgamecontroller.h"
#include <vector>
#include <map>
#include <string>
#include <random>

// A simplified Q-Learning agent instead of a full DQN to avoid heavy dependencies.
// It discretizes the state space and uses a Q-table for learning.
class QLearningAgent : public AbstractGameController {
public:
    QLearningAgent();
    virtual ~QLearningAgent() = default;

    PaddleAction getAction(const GameStateInfo& state) override;
    void update(const GameStateInfo& oldState, PaddleAction action, float reward, const GameStateInfo& newState) override;

private:
    // Discretize the continuous game state into a string representation
    std::string discretizeState(const GameStateInfo& state);

    std::map<std::string, std::vector<float>> qTable; // Q-table: state -> [Q(up), Q(down), Q(stay)]

    // Learning parameters
    const float alpha = 0.1f;   // Learning rate
    const float gamma = 0.9f;   // Discount factor
    float epsilon = 1.0f;     // Exploration rate
    const float epsilonDecay = 0.9995f; // Rate at which epsilon decays
    const float minEpsilon = 0.01f;

    // Random number generation for exploration
    std::mt19937 randomGenerator;
};

#endif // QLEARNINGAGENT_H
