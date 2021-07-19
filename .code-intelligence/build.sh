cmake -B build
cd .code-intelligence && cmake -B build -G Ninja
cmake --build .code-intelligence/build
cmake --build build
