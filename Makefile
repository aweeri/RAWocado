# Rawocado - RAW Image Editor Makefile
.PHONY: all build clean deps clean-deps distclean help

# Configuration
CC = x86_64-w64-mingw32-g++-posix
RC = x86_64-w64-mingw32-windres
CXXFLAGS = -O3 -ffast-math -std=c++17 -static -static-libgcc -static-libstdc++
JOBS ?= $(shell nproc 2>/dev/null || echo 4)
VENDOR_DIR = vendor

# Directories
IMGUI_DIR = $(VENDOR_DIR)/imgui
GLFW_DIR = $(VENDOR_DIR)/glfw
LIBRAW_DIR = $(VENDOR_DIR)/libraw
NFD_DIR = $(VENDOR_DIR)/nfd

# Include paths
INCLUDES = -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(GLFW_DIR)/include -I$(LIBRAW_DIR) -I$(NFD_DIR)/include -I$(VENDOR_DIR)

# Library paths
LIBPATHS = -L$(GLFW_DIR)/lib-mingw-w64 -L$(LIBRAW_DIR)/lib

# Libraries - use libraw.lib (import library for DLL) instead of static
LIBS = -lglfw3 -lgdi32 -lopengl32 -llibraw -lcomdlg32 -lole32 -luuid -lshell32 -pthread

# Source files
IMGUI_SOURCES = $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp
IMGUI_BACKEND_SOURCES = $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp
NFD_SOURCES = $(NFD_DIR)/nfd_common.c $(NFD_DIR)/nfd_win.cpp
MAIN_SOURCE = main.cpp
PROCESSING_SOURCE = processing.cpp
RESOURCE_SOURCE = rawocado.rc

# Object files (for parallel compilation)
BUILD_DIR = build
IMGUI_OBJS = $(addprefix $(BUILD_DIR)/, $(notdir $(IMGUI_SOURCES:.cpp=.o)))
IMGUI_BACKEND_OBJS = $(addprefix $(BUILD_DIR)/, $(notdir $(IMGUI_BACKEND_SOURCES:.cpp=.o)))
NFD_OBJS = $(BUILD_DIR)/nfd_common.o $(BUILD_DIR)/nfd_win.o
MAIN_OBJ = $(BUILD_DIR)/main.o
PROCESSING_OBJ = $(BUILD_DIR)/processing.o
RESOURCE_OBJ = $(BUILD_DIR)/rawocado_res.o
ALL_OBJS = $(MAIN_OBJ) $(PROCESSING_OBJ) $(RESOURCE_OBJ) $(IMGUI_OBJS) $(IMGUI_BACKEND_OBJS) $(NFD_OBJS)

# vpath for finding source files
vpath %.cpp $(IMGUI_DIR) $(IMGUI_DIR)/backends $(NFD_DIR)
vpath %.c $(NFD_DIR)

# Output
OUTPUT = RAWocado.exe
LIBRAW_DLL = $(LIBRAW_DIR)/bin/libraw.dll

# Dependency URLs
IMGUI_URL = https://github.com/ocornut/imgui/archive/refs/heads/master.zip
GLFW_URL = https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.bin.WIN64.zip
LIBRAW_URL = https://www.libraw.org/data/LibRaw-0.22.1-Win64.zip
NFD_URL = https://github.com/mlabbe/nativefiledialog/archive/refs/heads/master.zip
STB_IMAGE_WRITE_URL = https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h

# Check if dependencies exist
DEPS_EXIST = $(shell [ -d "$(IMGUI_DIR)/imgui.cpp" ] && echo "yes" || echo "no")

# Default target - build only if deps exist
all: build

# Build target - requires dependencies to exist
build: $(OUTPUT)

# Main executable - link object files
$(OUTPUT): $(ALL_OBJS)
	@echo "Linking executable..."
	$(CC) $(CXXFLAGS) -mwindows $(ALL_OBJS) $(LIBPATHS) $(LIBS) -o $(OUTPUT)
	@if [ -f "$(LIBRAW_DLL)" ]; then \
		echo "Copying required libraries..."; \
		cp $(LIBRAW_DLL) .; \
	fi
	@echo "Build successful! Run: ./rawocado.exe"

# Compile main.cpp
$(BUILD_DIR)/main.o: $(MAIN_SOURCE)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling main.cpp..."
	$(CC) $(CXXFLAGS) $(INCLUDES) -c $(MAIN_SOURCE) -o $@

# Compile processing.cpp
$(BUILD_DIR)/processing.o: $(PROCESSING_SOURCE)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling processing.cpp..."
	$(CC) $(CXXFLAGS) $(INCLUDES) -c $(PROCESSING_SOURCE) -o $@

# Compile Windows resources
$(BUILD_DIR)/rawocado_res.o: $(RESOURCE_SOURCE) rawocado.ico
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling Windows resources..."
	$(RC) $(RESOURCE_SOURCE) $@

# Compile imgui sources
$(BUILD_DIR)/imgui.o: $(IMGUI_DIR)/imgui.cpp
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CXXFLAGS) $(INCLUDES) -c $(IMGUI_DIR)/imgui.cpp -o $@

$(BUILD_DIR)/imgui_draw.o: $(IMGUI_DIR)/imgui_draw.cpp
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CXXFLAGS) $(INCLUDES) -c $(IMGUI_DIR)/imgui_draw.cpp -o $@

$(BUILD_DIR)/imgui_tables.o: $(IMGUI_DIR)/imgui_tables.cpp
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CXXFLAGS) $(INCLUDES) -c $(IMGUI_DIR)/imgui_tables.cpp -o $@

$(BUILD_DIR)/imgui_widgets.o: $(IMGUI_DIR)/imgui_widgets.cpp
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CXXFLAGS) $(INCLUDES) -c $(IMGUI_DIR)/imgui_widgets.cpp -o $@

# Compile imgui backend sources
$(BUILD_DIR)/imgui_impl_glfw.o: $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CXXFLAGS) $(INCLUDES) -c $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp -o $@

$(BUILD_DIR)/imgui_impl_opengl3.o: $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CXXFLAGS) $(INCLUDES) -c $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp -o $@

# Compile NFD sources
$(BUILD_DIR)/nfd_common.o: $(NFD_DIR)/nfd_common.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CXXFLAGS) $(INCLUDES) -c $(NFD_DIR)/nfd_common.c -o $@

$(BUILD_DIR)/nfd_win.o: $(NFD_DIR)/nfd_win.cpp
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CXXFLAGS) $(INCLUDES) -c $(NFD_DIR)/nfd_win.cpp -o $@

# Download and setup dependencies (only runs once)
deps: $(IMGUI_DIR)/imgui.cpp

$(IMGUI_DIR)/imgui.cpp:
	@echo "Setting up dependencies..."
	@mkdir -p $(VENDOR_DIR)/imgui $(VENDOR_DIR)/glfw $(VENDOR_DIR)/libraw $(VENDOR_DIR)/nfd
	@echo "Downloading ImGui..."
	@curl -sL $(IMGUI_URL) -o imgui.zip
	@unzip -q imgui.zip
	@cp -r imgui-master/* $(IMGUI_DIR)/
	@rm -rf imgui-master imgui.zip
	@echo "Downloading GLFW..."
	@curl -sL $(GLFW_URL) -o glfw.zip
	@unzip -q glfw.zip
	@cp -r glfw-3.4.bin.WIN64/* $(GLFW_DIR)/
	@rm -rf glfw-3.4.bin.WIN64 glfw.zip
	@echo "Downloading LibRaw..."
	@curl -sL $(LIBRAW_URL) -o libraw.zip
	@unzip -q libraw.zip
	@cp -r LibRaw-0.22.1/* $(LIBRAW_DIR)/
	@rm -rf LibRaw-0.22.1 libraw.zip
	@echo "Downloading Native File Dialog..."
	@curl -sL $(NFD_URL) -o nfd.zip
	@unzip -q nfd.zip
	@cp -r nativefiledialog-master/src/* $(NFD_DIR)/ 2>/dev/null || true
	@rm -rf nativefiledialog-master nfd.zip
	@echo "Downloading stb_image_write..."
	@curl -sL $(STB_IMAGE_WRITE_URL) -o $(VENDOR_DIR)/stb_image_write.h
	@echo "Dependencies downloaded successfully!"

# Clean targets
clean:
	@echo "Cleaning build artifacts..."
	@rm -f $(OUTPUT) libraw.dll
	@rm -rf $(BUILD_DIR)

clean-deps:
	@echo "Removing all vendor dependencies..."
	@rm -rf $(VENDOR_DIR)

distclean: clean clean-deps
	@echo "Full clean complete."

# Help target
help:
	@echo "Rawocado Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  deps           - Download and setup all dependencies (run this first)"
	@echo "  build          - Build the project (requires deps to be downloaded)"
	@echo "  all (default)  - Alias for 'build'"
	@echo "  clean          - Remove build artifacts (exe, dll, object files)"
	@echo "  clean-deps     - Remove all downloaded dependencies"
	@echo "  distclean      - Full clean (build + dependencies)"
	@echo "  help           - Show this help message"
	@echo ""
	@echo "Quick start:"
	@echo "  make deps             # Download dependencies first"
	@echo "  make -j4 build        # Build with 4 parallel jobs"
	@echo "  make -j build         # Build with auto-detected cores"
	@echo ""
	@echo "Note: Use 'make -j<N>' to compile with N parallel jobs for faster builds."
