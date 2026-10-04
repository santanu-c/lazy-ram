# lazy-ram -- builds with GCC 15+ on Linux or Apple clang on macOS (x86_64 only).

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O3 -Wall -Wextra
CPPFLAGS += -Iinclude -isystem third_party
LDFLAGS  += -pthread

PYTHON   ?= python3.12
VENV     := .venv
ARGS     ?= 0 0

BUILD    ?= build
TARGET   := $(BUILD)/lazy_ram
SRCS     := $(wildcard src/*.cpp)
OBJS     := $(SRCS:src/%.cpp=$(BUILD)/%.o)
DEPS     := $(OBJS:.o=.d)

.PHONY: all debug run venv run-python clean distclean

all: $(TARGET)

# Separate build dir so debug and release objects never mix.
debug:
	$(MAKE) BUILD=build/debug CXXFLAGS="-std=c++20 -O0 -g -Wall -Wextra"

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	mkdir -p $@

run: $(TARGET)
	./$(TARGET) $(ARGS)

$(VENV)/.installed: python/requirements.txt
	$(PYTHON) -m venv $(VENV)
	$(VENV)/bin/pip install --upgrade pip
	$(VENV)/bin/pip install -r python/requirements.txt
	touch $@

venv: $(VENV)/.installed

run-python: venv
	$(VENV)/bin/python python/lazy_ram.py $(ARGS)

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(VENV)

-include $(DEPS)
