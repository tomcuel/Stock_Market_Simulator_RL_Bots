//=======================================================================
// Machine-learning signal for the MLSignalBot: Python trains, C++ runs inference
//
//   scripts/train_signal_model.py  learns a logistic regression "will the price be higher in `horizon` samples?" from price series 
//                                  (a simulation run's price_samples_simu.csv, or real closes from Data/), and writes it to a small CSV (models/signal_model.csv by default)
//   SignalModel (below)            loads that CSV: inference is 5 features and a dot product, so it runs in microseconds inside the bot's own C++ loop: no Python process, no IPC, nothing slowing the market down
//
// The two sides must compute the features the exact same way, so they are defined once here and mirrored line by line in train_signal_model.py (NRT/test_bots.cpp pins values computed by the Python code to catch any drift) 
// For a price series p (oldest first, p_t the latest):
//   ret_1      p_t / p_{t-1} - 1
//   ret_5      p_t / p_{t-5} - 1
//   ma_gap_10  p_t / mean(p_{t-9} .. p_t) - 1
//   vol_10     population standard deviation of the last 10 one-step returns
//   rsi_14     (RSI - 50) / 50, RSI from the plain average gain and loss of the last 14 changes (100 when there was no loss), so the feature lies in [-1, 1]
// At least kSignalMinHistory prices are needed
//=======================================================================
#ifndef SIGNAL_MODEL_HPP
#define SIGNAL_MODEL_HPP

#include <array>
#include <deque>
#include <optional>
#include <string>

namespace sim {

constexpr std::size_t kSignalFeatureCount = 5;
constexpr std::size_t kSignalMinHistory = 15;
using SignalFeatures = std::array<double, kSignalFeatureCount>;

// names in model-file order, must match train_signal_model.py FEATURES
constexpr std::array<const char*, kSignalFeatureCount> kSignalFeatureNames = {"ret_1", "ret_5", "ma_gap_10", "vol_10", "rsi_14"};

// features of the latest point of `prices` (oldest first), or nothing if the history is too short or contains a non-positive price
std::optional<SignalFeatures> compute_signal_features(const std::deque<double>& prices);

class SignalModel{
public:
    // Reads the CSV written by train_signal_model.py. Returns false and explains why in `error`
    // (missing file, missing feature, feature names in a different order than kSignalFeatureNames)
    bool load(const std::string& path, std::string& error);

    // probability that the price is higher `horizon()` samples later, according to the model
    double probability_up(const SignalFeatures& features) const;

    int horizon() const { return horizon_; }
    bool loaded() const { return loaded_; }

private:
    bool loaded_{false};
    int horizon_{5};
    double bias_{0.0};
    SignalFeatures mean_{};
    SignalFeatures scale_{}; // standard deviation used for standardization (1 when it was 0)
    SignalFeatures weight_{};
};

} // namespace sim

#endif // SIGNAL_MODEL_HPP
