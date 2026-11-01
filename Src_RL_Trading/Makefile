CXX = g++
# -MMD -MP: every object also records which headers it includes (build/*.d), so changing a header
# rebuilds everything that uses it instead of silently linking stale objects
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -Iinclude -MMD -MP
LDFLAGS = -pthread

SRC_DIR = src
BUILD_DIR = build

# core matching engine, shared by the in-process demo (simulation.x) and the socket server (sim_server.x)
ENGINE_SOURCES = $(SRC_DIR)/order_book.cpp $(SRC_DIR)/matching_engine.cpp $(SRC_DIR)/order_registry.cpp $(SRC_DIR)/market_data.cpp $(SRC_DIR)/market_seed.cpp $(SRC_DIR)/market_recorder.cpp
ENGINE_OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(ENGINE_SOURCES))

.PHONY: all clean run server client

all: simulation.x sim_server.x sim_client.x

# simulation.x: in-process simulation in trading days (bots call the engine directly, no sockets). The rule-based strategies (bot.cpp) only depend on the MarketGateway interface, so the socket client links them too, see below
# the RL bots (rl_bot.cpp, simulation.x only) reach the Python policy server through policy_client.cpp and the client connection, fed by market_feed.cpp
STRATEGY_SOURCES = $(SRC_DIR)/bot.cpp
RL_SOURCES = $(SRC_DIR)/rl_bot.cpp $(SRC_DIR)/policy_client.cpp $(SRC_DIR)/market_feed.cpp
SIMULATION_SOURCES = $(ENGINE_SOURCES) $(STRATEGY_SOURCES) $(RL_SOURCES) $(SRC_DIR)/gateway.cpp $(SRC_DIR)/market_session.cpp $(SRC_DIR)/simulation.cpp $(SRC_DIR)/main.cpp
SIMULATION_OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SIMULATION_SOURCES)) $(BUILD_DIR)/net_protocol.o $(BUILD_DIR)/net_client_connection.o
simulation.x: $(SIMULATION_OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# sim_server.x: the exchange server, exposes the engine over the socket protocol
SERVER_OBJECTS = $(ENGINE_OBJECTS) $(BUILD_DIR)/net_protocol.o $(BUILD_DIR)/net_auth.o $(BUILD_DIR)/net_persistence.o \
                  $(BUILD_DIR)/net_simulation_server.o $(BUILD_DIR)/net_sim_server_main.o
sim_server.x: $(SERVER_OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# sim_client.x: interactive/piped client AND bot client, pure socket protocol, no engine linked in: the bots run the same rule-based strategy code as simulation.x, through a WireGateway instead of the engine (no RL bot)
CLIENT_OBJECTS = $(BUILD_DIR)/net_protocol.o $(BUILD_DIR)/net_client_connection.o $(BUILD_DIR)/net_wire_gateway.o \
                 $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(STRATEGY_SOURCES)) $(BUILD_DIR)/net_sim_client_main.o
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

-include $(wildcard $(BUILD_DIR)/*.d)
