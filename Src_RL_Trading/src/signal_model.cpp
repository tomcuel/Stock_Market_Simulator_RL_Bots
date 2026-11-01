#include "signal_model.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

namespace sim {

std::optional<SignalFeatures> compute_signal_features(const std::deque<double>& prices) {
    const std::size_t n = prices.size();
    if (n < kSignalMinHistory) {
        return std::nullopt;
    }   
    for (std::size_t i = n - kSignalMinHistory; i < n; ++i) {
        if (!(prices[i] > 0.0)) {
            return std::nullopt;
        }
    }
    auto at = [&](std::size_t back) {return prices[n - 1 - back];}; // at(0) = latest

    SignalFeatures f{};
    f[0] = at(0) / at(1) - 1.0;
    f[1] = at(0) / at(5) - 1.0;

    double sum = 0.0;
    for (std::size_t k = 0; k < 10; ++k) {
        sum += at(k);
    }
    f[2] = at(0) / (sum / 10.0) - 1.0;

    double returns[10];
    double mean_return = 0.0;
    for (std::size_t k = 0; k < 10; ++k) {
        returns[k] = at(k) / at(k + 1) - 1.0;
        mean_return += returns[k];
    }
    mean_return /= 10.0;
    double variance = 0.0;
    for (double r : returns) {
        variance += (r - mean_return) * (r - mean_return);
    }
    f[3] = std::sqrt(variance / 10.0);

    double gain = 0.0, loss = 0.0;
    for (std::size_t k = 0; k < 14; ++k) {
        double change = at(k) - at(k + 1);
        if (change > 0.0) {
            gain += change;
        } 
        else {
            loss -= change;
        }
    }
    double rsi = (loss == 0.0) ? 100.0 : 100.0 - 100.0 / (1.0 + (gain / 14.0) / (loss / 14.0));
    f[4] = (rsi - 50.0) / 50.0;
    return f;
}

bool SignalModel::load(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in.is_open()) {
        error = "cannot open " + path + " (train one with scripts/train_signal_model.py) ";
        return false;
    }
    std::array<bool, kSignalFeatureCount> seen{};
    bool has_bias = false;
    std::size_t next_feature = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::vector<std::string> cells;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) {
            cells.push_back(cell);
        }
        if (cells.size() < 2 || cells[0] == "kind") {
            continue; // header row
        }
        try {
            if (cells[0] == "meta" && cells[1] == "horizon" && cells.size() >= 3) {
                horizon_ = std::stoi(cells[2]);
            }
            else if (cells[0] == "bias" && cells.size() >= 5) {
                bias_ = std::stod(cells[4]);
                has_bias = true;
            }
            else if (cells[0] == "feature" && cells.size() >= 5) {
                if (next_feature >= kSignalFeatureCount || cells[1] != kSignalFeatureNames[next_feature]) {
                    error = "unexpected feature '" + cells[1] + "' in " + path + ", expected the order ret_1, ret_5, ma_gap_10, vol_10, rsi_14";
                    return false;
                }
                mean_[next_feature] = std::stod(cells[2]);
                double sd = std::stod(cells[3]);
                scale_[next_feature] = (sd > 0.0) ? sd : 1.0;
                weight_[next_feature] = std::stod(cells[4]);
                seen[next_feature] = true;
                ++next_feature;
            }
        }
        catch (const std::exception&) {
            error = "malformed line in " + path + ": " + line;
            return false;
        }
    }
    for (std::size_t i = 0; i < kSignalFeatureCount; ++i) {
        if (!seen[i]) {
            error = std::string("feature ") + kSignalFeatureNames[i] + " missing from " + path;
            return false;
        }
    }
    if (!has_bias) {
        error = "bias missing from " + path;
        return false;
    }
    loaded_ = true;
    return true;
}

double SignalModel::probability_up(const SignalFeatures& features) const
{
    double logit = bias_;
    for (std::size_t i = 0; i < kSignalFeatureCount; ++i) {
        logit += weight_[i] * (features[i] - mean_[i]) / scale_[i];
    }
    return 1.0 / (1.0 + std::exp(-logit));
}

} // namespace sim
