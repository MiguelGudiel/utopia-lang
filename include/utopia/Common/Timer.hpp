#pragma once
#include <chrono>
#include <string>
#include <iostream>

namespace utopia {

class ScopedTimer {
public:
    using Clock = std::chrono::high_resolution_clock;
    
    ScopedTimer(const std::string& name) 
        : name(name), start(Clock::now()) {}
    
    ~ScopedTimer() {
        auto end = Clock::now();
        auto ms = std::chrono::duration<double, std::milli>(end - start).count();
        std::cout << "[Timing] " << name << ": " << ms << " ms\n";
    }
    
private:
    std::string name;
    Clock::time_point start;
};

} // namespace utopia