#include <iostream>

#include "nrt_framework.hpp"

int main() {
    int passed = 0;
    int failed = 0;

    for (const auto& test : nrt::registry()) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
            ++passed;
        } catch (const nrt::AssertionFailure& e) {
            std::cout << "[FAIL] " << test.name << " : " << e.what() << "\n";
            ++failed;
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << test.name << " : unexpected exception: " << e.what() << "\n";
            ++failed;
        } catch (...) {
            std::cout << "[FAIL] " << test.name << " : unknown exception\n";
            ++failed;
        }
    }

    std::cout << "\n" << passed << " passed, " << failed << " failed, " << (passed + failed) << " total\n";
    return failed == 0 ? 0 : 1;
}
