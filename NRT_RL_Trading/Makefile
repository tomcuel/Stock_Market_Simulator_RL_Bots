CXX = g++
SIM_DIR = ../Src_Simulation
# -MMD -MP: header dependency tracking (build/*.d), a changed header rebuilds its users
# NRT_OUTPUT_DIR: every file a test writes goes under NRT/output/ (created on demand)
CXXFLAGS = -std=c++20 -O0 -g -Wall -Wextra -I. -I$(SIM_DIR)/include  -MMD -MP -DNRT_OUTPUT_DIR=\"$(CURDIR)/output\"
LDFLAGS = -pthread

BUILD_DIR = build
TARGET = nrt.x

# every engine source file except main.cpp (the CLI entry point, not needed for tests)
ENGINE_SOURCES = $(filter-out $(SIM_DIR)/src/main.cpp,$(wildcard $(SIM_DIR)/src/*.cpp))
# every net/ source except the two CLI entry points (sim_server_main.cpp, sim_client_main.cpp): tests link against SimulationServer/ClientConnection/etc. directly instead
NET_SOURCES = $(filter-out $(SIM_DIR)/src/net/sim_server_main.cpp $(SIM_DIR)/src/net/sim_client_main.cpp,$(wildcard $(SIM_DIR)/src/net/*.cpp))
TEST_SOURCES = $(wildcard test_*.cpp) runner.cpp
SOURCES = $(ENGINE_SOURCES) $(NET_SOURCES) $(TEST_SOURCES)

OBJECTS = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(notdir $(SOURCES)))

vpath %.cpp $(SIM_DIR)/src $(SIM_DIR)/src/net .

.PHONY: all test clean

all: $(TARGET)

test: $(TARGET)
	./$(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(wildcard $(BUILD_DIR)/*.d)