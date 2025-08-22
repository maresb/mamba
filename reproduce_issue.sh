#!/bin/bash

# Reproducer script for mamba repodata_record.json issue
# This script demonstrates the problem where installing from explicit lockfiles
# results in incomplete metadata in repodata_record.json

set -e

echo "=== Mamba Repodata Record Issue Reproducer ==="
echo "This script reproduces the issue where installing from explicit lockfiles"
echo "results in incomplete metadata in repodata_record.json"
echo ""

# Check if micromamba is available
if ! command -v micromamba &> /dev/null; then
    echo "Error: micromamba is not installed or not in PATH"
    echo "Please install micromamba first: https://mamba.readthedocs.io/en/latest/installation.html"
    exit 1
fi

echo "Using micromamba version: $(micromamba --version)"
echo ""

# Create a temporary directory for testing
TEMP_DIR=$(mktemp -d)
echo "Created temporary directory: $TEMP_DIR"
cd "$TEMP_DIR"

# Create a simple lockfile with explicit URL
echo "Creating test lockfile..."
cat > test.lock << 'EOF'
@EXPLICIT
https://conda.anaconda.org/conda-forge/noarch/ipykernel-6.30.1-pyh82676e8_0.conda#b0cc25825ce9212b8bee37829abad4d6
EOF

echo "Lockfile contents:"
cat test.lock
echo ""

# Create a clean conda environment for testing
ENV_NAME="test-repodata-issue"
echo "Creating conda environment: $ENV_NAME"
micromamba create -n "$ENV_NAME" -y

# Activate the environment
echo "Activating environment..."
eval "$(micromamba shell hook -s bash)"
micromamba activate "$ENV_NAME"

# Install from the lockfile
echo "Installing from lockfile..."
micromamba install -y -f test.lock

# Find the installed package
echo "Looking for installed package..."
PACKAGE_DIR=$(find "$CONDA_PREFIX/pkgs" -name "ipykernel-6.30.1-pyh82676e8_0" -type d | head -1)

if [ -z "$PACKAGE_DIR" ]; then
    echo "Error: Could not find installed package directory"
    exit 1
fi

echo "Found package at: $PACKAGE_DIR"

# Check if repodata_record.json exists
REPODATA_FILE="$PACKAGE_DIR/info/repodata_record.json"
if [ ! -f "$REPODATA_FILE" ]; then
    echo "Error: repodata_record.json not found"
    exit 1
fi

echo ""
echo "=== repodata_record.json contents ==="
cat "$REPODATA_FILE" | jq '.' 2>/dev/null || cat "$REPODATA_FILE"

echo ""
echo "=== Checking for the issue ==="

# Check if depends is empty (the main issue)
DEPENDS_COUNT=$(jq '.depends | length' "$REPODATA_FILE" 2>/dev/null || echo "0")
if [ "$DEPENDS_COUNT" -eq 0 ]; then
    echo "❌ ISSUE CONFIRMED: 'depends' field is empty or missing"
    echo "   This means the package metadata is incomplete"
else
    echo "✅ 'depends' field has $DEPENDS_COUNT items"
fi

# Check if timestamp is 0
TIMESTAMP=$(jq '.timestamp' "$REPODATA_FILE" 2>/dev/null || echo "0")
if [ "$TIMESTAMP" -eq 0 ]; then
    echo "❌ ISSUE CONFIRMED: 'timestamp' field is 0"
else
    echo "✅ 'timestamp' field is: $TIMESTAMP"
fi

# Check if sha256 is present
SHA256=$(jq -r '.sha256 // empty' "$REPODATA_FILE" 2>/dev/null || echo "")
if [ -z "$SHA256" ]; then
    echo "❌ ISSUE CONFIRMED: 'sha256' field is missing"
else
    echo "✅ 'sha256' field is present: ${SHA256:0:8}..."
fi

echo ""
echo "=== Summary ==="
if [ "$DEPENDS_COUNT" -eq 0 ] || [ "$TIMESTAMP" -eq 0 ] || [ -z "$SHA256" ]; then
    echo "The issue is confirmed. The repodata_record.json contains incomplete metadata."
    echo "This happens because mamba is using incomplete metadata from the URL"
    echo "instead of the complete metadata from the extracted package."
    echo ""
    echo "To fix this issue, apply the patch to libmamba/src/core/transaction.cpp"
    echo "that modifies the write_repodata_record method to prioritize"
    echo "metadata from the extracted package over URL metadata."
else
    echo "✅ No issues detected. The repodata_record.json contains complete metadata."
fi

echo ""
echo "Cleaning up..."
micromamba deactivate
micromamba env remove -n "$ENV_NAME" -y
cd /
rm -rf "$TEMP_DIR"

echo "Done."