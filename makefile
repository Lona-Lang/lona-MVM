ROOT ?= .
LLVM_CONFIG ?= llvm-config-18
CLANG ?= clang-18
LONA_IR ?= ../lona/build/lona-ir
LLVM_AS ?= llvm-as-18
CXX ?= clang++
OUT_DIR ?= build
EXPORT_DYNAMIC_FLAGS := -rdynamic

BASE_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags)
LLVM_LD_FLAGS := $(shell $(LLVM_CONFIG) --ldflags --system-libs --libs core orcjit native passes support bitreader executionengine nativecodegen)
CXXFLAGS := $(BASE_CXXFLAGS) -std=c++20 -g -Wall
INCLUDE_PATHS := -I$(ROOT)/src

SOURCE_FILES := $(shell find $(ROOT)/src -name "*.cc" | sort)
MAIN_SOURCE := $(ROOT)/src/main.cc
LIBRARY_SOURCE_FILES := $(filter-out $(MAIN_SOURCE),$(SOURCE_FILES))
MAIN_OBJECT := $(patsubst %.cc,$(OUT_DIR)/%.o,$(MAIN_SOURCE))
LIBRARY_OBJECTS := $(patsubst %.cc,$(OUT_DIR)/%.o,$(LIBRARY_SOURCE_FILES))
TARGET := $(OUT_DIR)/mvm
RUNTIME_MEMORY_TEST_SOURCE := $(ROOT)/tests/runtime_memory_test.cc
RUNTIME_MEMORY_TEST_OBJECT := $(patsubst %.cc,$(OUT_DIR)/%.o,$(RUNTIME_MEMORY_TEST_SOURCE))
RUNTIME_MEMORY_OBJECT := $(OUT_DIR)/src/mvm/runtime_memory.o
RUNTIME_MEMORY_TEST_TARGET := $(OUT_DIR)/runtime_memory_test
EXAMPLE_LO := $(ROOT)/examples/hello.lo
EXAMPLE_BC := $(OUT_DIR)/examples/hello.bc
STATIC_ARRAY_OK_LO := $(ROOT)/examples/static_array_ok.lo
STATIC_ARRAY_OK_BC := $(OUT_DIR)/examples/static_array_ok.bc
STATIC_ARRAY_OOB_LO := $(ROOT)/examples/static_array_oob.lo
STATIC_ARRAY_OOB_BC := $(OUT_DIR)/examples/static_array_oob.bc
NO_DEBUG_BC := $(OUT_DIR)/examples/hello_no_debug.bc
INVALID_BC := $(OUT_DIR)/examples/invalid_ptr_cast.bc
RUNTIME_ARRAY_API_LO := $(ROOT)/examples/runtime_array_api.lo
RUNTIME_ARRAY_API_BC := $(OUT_DIR)/examples/runtime_array_api.bc
RUNTIME_ARRAY_OOB_LO := $(ROOT)/examples/runtime_array_oob.lo
RUNTIME_ARRAY_OOB_BC := $(OUT_DIR)/examples/runtime_array_oob.bc
RUNTIME_ARGV_LO := $(ROOT)/examples/runtime_argv.lo
RUNTIME_ARGV_BC := $(OUT_DIR)/examples/runtime_argv.bc
INVALID_RAW_MALLOC_LO := $(ROOT)/examples/runtime_raw_malloc.lo
INVALID_RAW_MALLOC_BC := $(OUT_DIR)/examples/runtime_raw_malloc.bc

.PHONY: all clean test

all: $(TARGET)

test: $(TARGET) $(RUNTIME_MEMORY_TEST_TARGET) $(EXAMPLE_BC) \
	$(STATIC_ARRAY_OK_BC) $(STATIC_ARRAY_OOB_BC) $(NO_DEBUG_BC) \
	$(INVALID_BC) $(RUNTIME_ARRAY_API_BC) $(RUNTIME_ARRAY_OOB_BC) \
	$(RUNTIME_ARGV_BC) \
	$(INVALID_RAW_MALLOC_BC)
	$(RUNTIME_MEMORY_TEST_TARGET)
	$(TARGET) $(EXAMPLE_BC)
	$(TARGET) $(STATIC_ARRAY_OK_BC)
	$(TARGET) $(RUNTIME_ARRAY_API_BC)
	$(TARGET) $(RUNTIME_ARGV_BC) -- foo
	! $(TARGET) $(STATIC_ARRAY_OOB_BC)
	! $(TARGET) $(NO_DEBUG_BC)
	! $(TARGET) $(INVALID_BC)
	! $(TARGET) $(RUNTIME_ARRAY_OOB_BC)
	! $(TARGET) $(INVALID_RAW_MALLOC_BC)

$(TARGET): $(MAIN_OBJECT) $(LIBRARY_OBJECTS)
	mkdir -p $(dir $@)
	$(CXX) $^ $(CXXFLAGS) $(INCLUDE_PATHS) $(LLVM_LD_FLAGS) \
		$(EXPORT_DYNAMIC_FLAGS) -o $@

$(RUNTIME_MEMORY_TEST_TARGET): $(RUNTIME_MEMORY_OBJECT) $(RUNTIME_MEMORY_TEST_OBJECT)
	mkdir -p $(dir $@)
	$(CXX) $^ $(CXXFLAGS) $(INCLUDE_PATHS) -o $@

$(OUT_DIR)/%.d: %.cc makefile
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDE_PATHS) -MM $< -MT $(@:.d=.o) > $@

$(OUT_DIR)/%.o: %.cc makefile
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDE_PATHS) -c $< -o $@

$(EXAMPLE_BC): $(EXAMPLE_LO) makefile
	mkdir -p $(dir $@)
	$(LONA_IR) --emit linked-bc --verify-ir -g $< $@

$(STATIC_ARRAY_OK_BC): $(STATIC_ARRAY_OK_LO) makefile
	mkdir -p $(dir $@)
	$(LONA_IR) --emit linked-bc --verify-ir -g $< $@

$(STATIC_ARRAY_OOB_BC): $(STATIC_ARRAY_OOB_LO) makefile
	mkdir -p $(dir $@)
	$(LONA_IR) --emit linked-bc --verify-ir -g $< $@

$(NO_DEBUG_BC): $(EXAMPLE_LO) makefile
	mkdir -p $(dir $@)
	$(LONA_IR) --emit linked-bc --verify-ir $< $@

$(INVALID_BC): $(ROOT)/examples/invalid_ptr_cast.ll makefile
	mkdir -p $(dir $@)
	$(LLVM_AS) $< -o $@

$(RUNTIME_ARRAY_API_BC): $(RUNTIME_ARRAY_API_LO) makefile
	mkdir -p $(dir $@)
	$(LONA_IR) --emit linked-bc --verify-ir -g $< $@

$(RUNTIME_ARRAY_OOB_BC): $(RUNTIME_ARRAY_OOB_LO) makefile
	mkdir -p $(dir $@)
	$(LONA_IR) --emit linked-bc --verify-ir -g $< $@

$(RUNTIME_ARGV_BC): $(RUNTIME_ARGV_LO) makefile
	mkdir -p $(dir $@)
	$(LONA_IR) --emit linked-bc --verify-ir -g $< $@

$(INVALID_RAW_MALLOC_BC): $(INVALID_RAW_MALLOC_LO) makefile
	mkdir -p $(dir $@)
	$(LONA_IR) --emit linked-bc --verify-ir -g $< $@

ifneq ($(filter clean,$(MAKECMDGOALS)),clean)
-include $(MAIN_OBJECT:.o=.d)
-include $(LIBRARY_OBJECTS:.o=.d)
-include $(RUNTIME_MEMORY_TEST_OBJECT:.o=.d)
endif

clean:
	rm -rf $(OUT_DIR)
