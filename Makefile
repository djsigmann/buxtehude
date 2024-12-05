BUILD_DIR := ./build
BUXTEHUDE_INCLUDE_DIR := include
INCLUDE_DIRS := $(BUXTEHUDE_INCLUDE_DIR)
INCLUDE := $(addprefix -I,$(INCLUDE_DIRS))
LIBRARIES := -lfmt -levent_core -levent_pthreads
CXXFLAGS := -std=c++20
CPPFLAGS := $(INCLUDE) -MMD -MP
LDFLAGS := $(LIBRARIES) -dynamiclib -rpath /usr/local/lib

# Build library

BUXTEHUDE_TARGET := libbuxtehude.dylib
BUXTEHUDE_SOURCE := $(wildcard src/*.cpp)
BUXTEHUDE_OBJECTS := $(BUXTEHUDE_SOURCE:%.cpp=$(BUILD_DIR)/%.o)
BUXTEHUDE_DEPENDENCIES := $(BUXTEHUDE_OBJECTS:%.o=%.d)

$(BUXTEHUDE_TARGET): $(BUXTEHUDE_OBJECTS)
	$(CXX) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

# Build tests

TEST_STREAM_TARGET := stream-test
TEST_STREAM_SOURCE := tests/stream-test.cpp
TEST_STREAM_OBJECTS := $(TEST_STREAM_SOURCE:%.cpp=$(BUILD_DIR)/%.o)
TEST_STREAM_DEPENDENCIES := $(TEST_STREAM_OBJECTS:%.o=%.d)
TEST_STREAM_LDFLAGS := -L. -lbuxtehude

$(TEST_STREAM_TARGET): $(TEST_STREAM_OBJECTS)
	$(CXX) $(TEST_STREAM_LDFLAGS) $^ -o $@

TEST_VALIDATE_TARGET := valid-test
TEST_VALIDATE_SOURCE := tests/valid-test.cpp
TEST_VALIDATE_OBJECTS := $(TEST_VALIDATE_SOURCE:%.cpp=$(BUILD_DIR)/%.o)
TEST_VALIDATE_DEPENDENCIES := $(TEST_VALIDATE_OBJECTS:%.o=%.d)
TEST_VALIDATE_LDFLAGS := -L. -lbuxtehude

$(TEST_VALIDATE_TARGET): $(TEST_VALIDATE_OBJECTS)
	$(CXX) $(TEST_STREAM_LDFLAGS) $^ -o $@

test: $(TEST_STREAM_TARGET) $(TEST_VALIDATE_TARGET)
	@echo "Running tests..."
	./$(TEST_STREAM_TARGET) && ./$(TEST_VALIDATE_TARGET)

# Rudimentary install for now

install: $(BUXTEHUDE_TARGET)
	cp $(BUXTEHUDE_TARGET) /usr/local/lib/
	cp -r $(BUXTEHUDE_INCLUDE_DIR) /usr/local/include/buxtehude

uninstall:
	rm /usr/local/lib/$(BUXTEHUDE_TARGET)
	rm -r /usr/local/include/buxtehude

.PHONY: clean
clean:
	rm -r $(BUILD_DIR)

-include $(BUXTEHUDE_DEPENDENCIES) $(TEST_STREAM_DEPENDENCIES)
