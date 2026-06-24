#!/bin/bash

compile_cmake() {
    local directory="$1"
    echo "Compiling CMake project in: $directory"
    cd "$directory"
    mkdir -p build  # Create build directory if not exists
    cd build
    local build_type="Release"
    local cmake_extra_args=()
    if [[ "${SANITIZE:-0}" == "1" ]]; then
        build_type="RelWithDebInfo"
        local sanitize_flags="-fsanitize=address -fno-omit-frame-pointer -g"
        cmake_extra_args+=("-DCMAKE_CXX_FLAGS=${sanitize_flags}")
        cmake_extra_args+=("-DCMAKE_EXE_LINKER_FLAGS=${sanitize_flags}")
        cmake_extra_args+=("-DCMAKE_SHARED_LINKER_FLAGS=${sanitize_flags}")
    fi
    cmake -DCMAKE_BUILD_TYPE="$build_type" "${cmake_extra_args[@]}" ..
    make -j1
    cd ../..
}

export -f compile_cmake  # Export function for parallel execution

base_directory="."
nproc=$(nproc)

# Find directories containing CMakeLists.txt and run compile_cmake in parallel
find "$base_directory" -mindepth 1 -maxdepth 1 -type d -exec test -f "{}/CMakeLists.txt" \; -print | \
    xargs -I{} -P "$nproc" bash -c 'compile_cmake "$@"' _ {}
