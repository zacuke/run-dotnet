#!/bin/bash

# Build script for run-node project
set -e  # Exit on any error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
BUILD_DIR=".build"
BUILD_TYPE="Release"  # Can be "Debug" or "Release"
NUM_JOBS=$(nproc)

# Function to print colored output
print_status() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if required tools are installed
check_dependencies() {
    local missing_deps=()
    
    if ! command -v cmake &> /dev/null; then
        missing_deps+=("cmake")
    fi
    
    if ! command -v make &> /dev/null; then
        missing_deps+=("make")
    fi
    
    if [ ${#missing_deps[@]} -ne 0 ]; then
        print_error "Missing dependencies: ${missing_deps[*]}"
        exit 1
    fi
}

# Clean build directory
clean_build() {
    if [ -d "$BUILD_DIR" ]; then
        print_status "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
    fi
}

# Configure the project
configure_project() {
    print_status "Configuring project with CMake..."
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    cmake .. \
        -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_CXX_STANDARD=17 \
        -DCMAKE_CXX_STANDARD_REQUIRED=ON
    
    cd ..
}

# Build the project
build_project() {
    print_status "Building project with $NUM_JOBS jobs..."
    cd "$BUILD_DIR"
    make -j$NUM_JOBS
}

# Run tests (if you add them later)
run_tests() {
    if [ -f "$BUILD_DIR/run-node-tests" ]; then
        print_status "Running tests..."
        ./run-node-tests
    fi
}

# Install the executable
install_project() {
    print_status "Installing executable..."
    cd "$BUILD_DIR"
    sudo make install
}

# Main build process
main() {
    print_status "Starting build process..."
    
    # Parse command line arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            --clean|-c)
                clean_build
                shift
                ;;
            --debug|-d)
                BUILD_TYPE="Debug"
                shift
                ;;
            --install|-i)
                INSTALL=true
                shift
                ;;
            --help|-h)
                echo "Usage: ./build.sh [options]"
                echo "Options:"
                echo "  -c, --clean    Clean build directory before building"
                echo "  -d, --debug    Build in debug mode"
                echo "  -i, --install  Install after building"
                echo "  -h, --help     Show this help message"
                exit 0
                ;;
            *)
                print_error "Unknown option: $1"
                exit 1
                ;;
        esac
    done
    
    check_dependencies
    
    if [ "$CLEAN" = true ]; then
        clean_build
    fi
    
    configure_project
    build_project
    
    # Optional: run tests if they exist
    run_tests
    
    if [ "$INSTALL" = true ]; then
        install_project
    fi
    
    print_status "Build completed successfully!"
    print_status "Executable location: $BUILD_DIR/run-node"
}

# Run main function with all arguments
main "$@"