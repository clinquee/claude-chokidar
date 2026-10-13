CXX ?= clang++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -pthread -flto

SRC_DIR = src
TEST_DIR = tests
BUILD_DIR = build

SRCS = $(SRC_DIR)/safety.cpp \
       $(SRC_DIR)/config.cpp \
       $(SRC_DIR)/chrome_finder.cpp \
       $(SRC_DIR)/net.cpp \
       $(SRC_DIR)/cdp.cpp \
       $(SRC_DIR)/health_server.cpp \
       $(SRC_DIR)/bot.cpp

MAIN_SRC = $(SRC_DIR)/main.cpp
TEST_SRC = $(TEST_DIR)/test_main.cpp

TARGET = claude-chokidar
TEST_TARGET = $(BUILD_DIR)/test_runner

OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SRCS))
MAIN_OBJ = $(BUILD_DIR)/main.o

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(MAIN_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(TEST_TARGET): $(OBJS) $(TEST_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(OBJS) $(TEST_SRC) -o $@

test: $(TEST_TARGET)
	@echo "Running C++ test suite..."
	@./$(TEST_TARGET)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

.PHONY: all test clean
