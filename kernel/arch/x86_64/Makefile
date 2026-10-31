# Written by [@saphhic](https://github.com/saphhic)
# Date: October 30th 2026
# Description: Compiles and archives routines specific to the x86-64 architecture.

# Written by [@saphhic](https://github.com/saphhic)  
# Date: 29 July 2026

# Makefile for x86_64 architecture source files
# doesnt compile the kernel, this is just to test
# if the source files compile correctly


CXX     := g++
ASM     := gcc

CXXFLAGS := -std=c++20 \
            -m64 \
            -mcmodel=kernel \
            -mno-red-zone \
            -fno-pic \
            -fno-pie \
            -ffreestanding \
            -fno-exceptions \
            -fno-rtti \
            -fno-threadsafe-statics \
            -fno-use-cxa-atexit \
            -Wall \
            -Wextra \
            -I. \
            -I../../ \
            -I../../include

ASMFLAGS := -m64 -x assembler -c

BUILD_DIR := bin

CPP_SOURCES := $(shell find . -name "*.cpp")
COMMON_CPP_SOURCES := $(shell find ../common -name "*.cpp")
ASM_SOURCES := $(shell find -name "*.S")

CPP_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(CPP_SOURCES))
COMMON_CPP_OBJECTS := $(patsubst ../common/%.cpp,$(BUILD_DIR)/common/%.o,$(COMMON_CPP_SOURCES))
ASM_OBJECTS := $(patsubst %.S,$(BUILD_DIR)/%.o,$(ASM_SOURCES))

OBJECTS := $(CPP_OBJECTS) $(COMMON_CPP_OBJECTS) $(ASM_OBJECTS)

all: $(OBJECTS)
	@echo ""
	@echo "==============================="
	@echo "Compiled Successfully"
	@echo "==============================="


$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/common/%.o: ../common/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(ASM) $(ASMFLAGS) $< -o $@

clean:
	-rm -rf $(BUILD_DIR)/*

.PHONY: all clean
