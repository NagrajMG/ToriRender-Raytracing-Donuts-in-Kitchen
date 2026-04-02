cd "$(dirname "$0")/.."

echo "Formatting codebase..."
find . -type d \( -name .git -o -name build \) -prune -o \
     \( -name "*.cpp" -o -name "*.hpp" \) -print | xargs clang-format -i -style=file

echo "Done."