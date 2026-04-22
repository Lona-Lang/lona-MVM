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
CXXFLAGS := $(BASE_CXXFLAGS) -std=c++20 -g -Wall -fno-omit-frame-pointer
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
GC_ROOT_SCAN_TEST_SOURCE := $(ROOT)/tests/gc_root_scan_test.cc
GC_ROOT_SCAN_TEST_OBJECT := $(patsubst %.cc,$(OUT_DIR)/%.o,$(GC_ROOT_SCAN_TEST_SOURCE))
GC_ROOT_SCAN_TEST_TARGET := $(OUT_DIR)/gc_root_scan_test
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
RUNTIME_ARRAY_API_MBC := $(OUT_DIR)/examples/runtime_array_api_mbc.bc
RUNTIME_ARRAY_OOB_LO := $(ROOT)/examples/runtime_array_oob.lo
RUNTIME_ARRAY_OOB_BC := $(OUT_DIR)/examples/runtime_array_oob.bc
RUNTIME_ARGV_LO := $(ROOT)/examples/runtime_argv.lo
RUNTIME_ARGV_BC := $(OUT_DIR)/examples/runtime_argv.bc
MANAGED_STATE_LO := $(ROOT)/examples/managed_state.lo
MANAGED_STATE_BC := $(OUT_DIR)/examples/managed_state.bc
MANAGED_DISPATCH_C := $(ROOT)/examples/managed_dispatch.c
MANAGED_DISPATCH_BC := $(OUT_DIR)/examples/managed_dispatch.bc
INVALID_RAW_MALLOC_LO := $(ROOT)/examples/runtime_raw_malloc.lo
INVALID_RAW_MALLOC_BC := $(OUT_DIR)/examples/runtime_raw_malloc.bc
INVALID_ELEMENT_ADDRESS_LO := $(ROOT)/examples/managed_invalid_element_address.lo
INVALID_ELEMENT_ADDRESS_STDERR := $(OUT_DIR)/examples/managed_invalid_element_address.stderr
IR_PIPELINE_DEMO_LO := $(ROOT)/examples/ir_pipeline_demo.lo
IR_PIPELINE_DEMO_RAW_LL := $(OUT_DIR)/examples/ir_pipeline_demo.before.ll
IR_PIPELINE_DEMO_BC := $(OUT_DIR)/examples/ir_pipeline_demo.bc
IR_PIPELINE_DEMO_AFTER_LL := $(OUT_DIR)/examples/ir_pipeline_demo.after.ll
GC_ROOT_SCAN_LO := $(ROOT)/examples/gc_root_scan.lo
GC_ROOT_SCAN_BC := $(OUT_DIR)/examples/gc_root_scan.bc

.PHONY: all clean test ir-demo

all: $(TARGET)

ir-demo: $(TARGET) $(IR_PIPELINE_DEMO_RAW_LL) $(IR_PIPELINE_DEMO_AFTER_LL)

test: $(TARGET) $(RUNTIME_MEMORY_TEST_TARGET) $(GC_ROOT_SCAN_TEST_TARGET) $(EXAMPLE_BC) \
	$(STATIC_ARRAY_OK_BC) $(STATIC_ARRAY_OOB_BC) $(NO_DEBUG_BC) \
	$(INVALID_BC) $(RUNTIME_ARRAY_API_BC) $(RUNTIME_ARRAY_API_MBC) \
	$(RUNTIME_ARRAY_OOB_BC) $(GC_ROOT_SCAN_BC) \
	$(RUNTIME_ARGV_BC) $(MANAGED_STATE_BC) $(MANAGED_DISPATCH_BC) \
	$(INVALID_RAW_MALLOC_BC) $(INVALID_ELEMENT_ADDRESS_STDERR)
	$(RUNTIME_MEMORY_TEST_TARGET)
	$(GC_ROOT_SCAN_TEST_TARGET) $(GC_ROOT_SCAN_BC)
	$(TARGET) --dump-ir $(EXAMPLE_BC) | rg 'llvm\.experimental\.gc\.statepoint|gc "statepoint-example"'
	$(TARGET) -O1 --dump-ir $(MANAGED_STATE_BC) | rg 'mvm\.managed\.signature|arg0=array'
	$(TARGET) -O1 --dump-ir $(MANAGED_STATE_BC) | rg 'ptr addrspace\(1\)|llvm\.experimental\.gc\.relocate\.p1'
	$(TARGET) -O1 --dump-ir $(MANAGED_DISPATCH_BC) | rg '@middle\.__mvm\.arg0_raw|@middle\.__mvm\.arg0_array'
	$(TARGET) -O1 --dump-ir $(MANAGED_DISPATCH_BC) | rg '@leaf\.__mvm\.arg0_raw|@leaf\.__mvm\.arg0_array'
	$(TARGET) -O1 --dump-ir $(RUNTIME_ARRAY_API_MBC) | rg 'declare .*@__mvm_array_length\(ptr addrspace\(1\)\)|declare .*@__mvm_array_free\(ptr addrspace\(1\)\)'
	$(TARGET) -O1 --dump-ir $(RUNTIME_ARRAY_API_MBC) | rg 'ptr addrspace\(1\)|llvm\.experimental\.gc\.relocate\.p1'
	$(TARGET) -O1 --dump-ir $(RUNTIME_ARRAY_API_MBC) | rg '!mvm\.gc\.module = !\{|!mvm\.gc\.function = !\{|!mvm\.gc\.statepoint|!mvm\.gc\.relocate'
	$(TARGET) $(EXAMPLE_BC)
	$(TARGET) $(STATIC_ARRAY_OK_BC)
	$(TARGET) $(RUNTIME_ARRAY_API_BC)
	$(TARGET) $(RUNTIME_ARRAY_API_MBC)
	$(TARGET) $(MANAGED_STATE_BC)
	$(TARGET) $(MANAGED_DISPATCH_BC)
	$(TARGET) $(RUNTIME_ARGV_BC) -- foo
	! $(TARGET) -O0 $(EXAMPLE_BC) > /dev/null 2> $(OUT_DIR)/examples/reject_o0.stderr
	grep -q 'managed mode requires -O1 or higher; -O0 is not supported' $(OUT_DIR)/examples/reject_o0.stderr
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

$(GC_ROOT_SCAN_TEST_TARGET): $(LIBRARY_OBJECTS) $(GC_ROOT_SCAN_TEST_OBJECT)
	mkdir -p $(dir $@)
	$(CXX) $^ $(CXXFLAGS) $(INCLUDE_PATHS) $(LLVM_LD_FLAGS) \
		$(EXPORT_DYNAMIC_FLAGS) -o $@

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

$(RUNTIME_ARRAY_API_MBC): $(RUNTIME_ARRAY_API_LO) makefile
	mkdir -p $(dir $@)
	$(LONA_IR) --emit mbc --verify-ir -g $< $@

$(RUNTIME_ARRAY_OOB_BC): $(RUNTIME_ARRAY_OOB_LO) makefile
	mkdir -p $(dir $@)
	$(LONA_IR) --emit linked-bc --verify-ir -g $< $@

$(RUNTIME_ARGV_BC): $(RUNTIME_ARGV_LO) makefile
	mkdir -p $(dir $@)
	$(LONA_IR) --emit linked-bc --verify-ir -g $< $@

$(MANAGED_STATE_BC): $(MANAGED_STATE_LO) makefile
	mkdir -p $(dir $@)
	$(LONA_IR) --emit linked-bc --verify-ir -g $< $@

$(MANAGED_DISPATCH_BC): $(MANAGED_DISPATCH_C) makefile
	mkdir -p $(dir $@)
	$(CLANG) -g -O1 -emit-llvm -c $< -o $@

$(INVALID_RAW_MALLOC_BC): $(INVALID_RAW_MALLOC_LO) makefile
	mkdir -p $(dir $@)
	$(LONA_IR) --emit linked-bc --verify-ir -g $< $@

$(INVALID_ELEMENT_ADDRESS_STDERR): $(INVALID_ELEMENT_ADDRESS_LO) makefile
	mkdir -p $(dir $@)
	! $(LONA_IR) --emit mbc --verify-ir -g $< $(OUT_DIR)/examples/managed_invalid_element_address.bc > /dev/null 2> $@
	grep -q 'managed mode does not allow taking the address of an element from an indexable pointer' $@

$(IR_PIPELINE_DEMO_RAW_LL): $(IR_PIPELINE_DEMO_LO) makefile
	mkdir -p $(dir $@)
	$(LONA_IR) --emit ir --verify-ir -g $< $@

$(IR_PIPELINE_DEMO_BC): $(IR_PIPELINE_DEMO_LO) makefile
	mkdir -p $(dir $@)
	$(LONA_IR) --emit mbc --verify-ir -g $< $@

$(GC_ROOT_SCAN_BC): $(GC_ROOT_SCAN_LO) makefile
	mkdir -p $(dir $@)
	$(LONA_IR) --emit mbc --verify-ir -g $< $@

$(IR_PIPELINE_DEMO_AFTER_LL): $(IR_PIPELINE_DEMO_BC) $(TARGET)
	mkdir -p $(dir $@)
	$(TARGET) -O1 --dump-ir $< > $@

ifneq ($(filter clean,$(MAKECMDGOALS)),clean)
-include $(MAIN_OBJECT:.o=.d)
-include $(LIBRARY_OBJECTS:.o=.d)
-include $(RUNTIME_MEMORY_TEST_OBJECT:.o=.d)
-include $(GC_ROOT_SCAN_TEST_OBJECT:.o=.d)
endif

clean:
	rm -rf $(OUT_DIR)
