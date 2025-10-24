# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -g -I./common
LDFLAGS = -pthread

# Directories
COMMON_DIR = common
NAME_SERVER_DIR = name_server
STORAGE_SERVER_DIR = storage_server
CLIENT_DIR = client
BUILD_DIR = build
BIN_DIR = bin

# Common source files
COMMON_SRCS = $(COMMON_DIR)/protocol.c $(COMMON_DIR)/utils.c
COMMON_OBJS = $(BUILD_DIR)/protocol.o $(BUILD_DIR)/utils.o

# Targets
all: directories $(BIN_DIR)/name_server $(BIN_DIR)/storage_server $(BIN_DIR)/client

# Create necessary directories
directories:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BIN_DIR)

# Compile common object files
$(BUILD_DIR)/protocol.o: $(COMMON_DIR)/protocol.c $(COMMON_DIR)/protocol.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/utils.o: $(COMMON_DIR)/utils.c $(COMMON_DIR)/utils.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Name Server
$(BUILD_DIR)/name_server.o: $(NAME_SERVER_DIR)/name_server.c $(NAME_SERVER_DIR)/name_server.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/name_server: $(BUILD_DIR)/name_server.o $(COMMON_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Storage Server (to be implemented)
$(BUILD_DIR)/storage_server.o: $(STORAGE_SERVER_DIR)/storage_server.c $(STORAGE_SERVER_DIR)/storage_server.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/storage_server: $(BUILD_DIR)/storage_server.o $(COMMON_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Client (to be implemented)
$(BUILD_DIR)/client.o: $(CLIENT_DIR)/client.c $(CLIENT_DIR)/client.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/client: $(BUILD_DIR)/client.o $(COMMON_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Protocol test
test_protocol: $(COMMON_DIR)/test_protocol.c $(COMMON_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $(BIN_DIR)/test_protocol $(LDFLAGS)
	./$(BIN_DIR)/test_protocol

# Clean build files
clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(BIN_DIR)

# Clean and rebuild
rebuild: clean all

# Install (optional, copies binaries to system location)
install: all
	@echo "Installing binaries to /usr/local/bin..."
	@sudo cp $(BIN_DIR)/name_server /usr/local/bin/
	@sudo cp $(BIN_DIR)/storage_server /usr/local/bin/
	@sudo cp $(BIN_DIR)/client /usr/local/bin/

# Uninstall
uninstall:
	@echo "Removing binaries from /usr/local/bin..."
	@sudo rm -f /usr/local/bin/name_server
	@sudo rm -f /usr/local/bin/storage_server
	@sudo rm -f /usr/local/bin/client

# Help
help:
	@echo "Available targets:"
	@echo "  all             - Build all components (default)"
	@echo "  clean           - Remove all build artifacts"
	@echo "  rebuild         - Clean and rebuild everything"
	@echo "  test_protocol   - Build and run protocol tests"
	@echo "  install         - Install binaries to system"
	@echo "  uninstall       - Remove binaries from system"
	@echo "  help            - Show this help message"
	@echo ""
	@echo "Individual components:"
	@echo "  $(BIN_DIR)/name_server      - Build name server"
	@echo "  $(BIN_DIR)/storage_server   - Build storage server"
	@echo "  $(BIN_DIR)/client           - Build client"

.PHONY: all directories clean rebuild install uninstall help test_protocol
