CXX = g++
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -Iinclude
LDFLAGS = -pthread

SRC_DIR = src
BUILD_DIR = build

# core matching engine, shared by the in-process demo (simulation.x) and the socket server (sim_server.x)
ENGINE_SOURCES = $(SRC_DIR)/order_book.cpp $(SRC_DIR)/matching_engine.cpp $(SRC_DIR)/order_registry.cpp $(SRC_DIR)/market_data.cpp $(SRC_DIR)/market_seed.cpp $(SRC_DIR)/market_recorder.cpp
ENGINE_OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(ENGINE_SOURCES))

.PHONY: all clean run server client

all: simulation.x sim_server.x sim_client.x

# simulation.x: in-process demo/benchmark (bots call the engine directly, no sockets)
SIMULATION_SOURCES = $(ENGINE_SOURCES) $(SRC_DIR)/bot.cpp $(SRC_DIR)/simulation.cpp $(SRC_DIR)/main.cpp
SIMULATION_OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SIMULATION_SOURCES))
simulation.x: $(SIMULATION_OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# sim_server.x: the exchange server, exposes the engine over the socket protocol
SERVER_OBJECTS = $(ENGINE_OBJECTS) $(BUILD_DIR)/net_protocol.o $(BUILD_DIR)/net_auth.o $(BUILD_DIR)/net_persistence.o \
                  $(BUILD_DIR)/net_simulation_server.o $(BUILD_DIR)/net_sim_server_main.o
sim_server.x: $(SERVER_OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# sim_client.x: interactive/piped client and bot client: pure socket protocol, no engine linked in
CLIENT_OBJECTS = $(BUILD_DIR)/net_protocol.o $(BUILD_DIR)/net_client_connection.o $(BUILD_DIR)/net_sim_client_main.o
sim_client.x: $(CLIENT_OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/net_%.o: $(SRC_DIR)/net/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: simulation.x
	./simulation.x

server: sim_server.x
	./sim_server.x

clean:
	rm -rf $(BUILD_DIR) simulation.x sim_server.x sim_client.x
