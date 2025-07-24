#!/bin/bash

# Script to update Doxygen documentation for the tutorial

echo "Generating Doxygen documentation..."
cd ../doxygen
make

echo "Copying documentation to tutorial public directory..."
mkdir -p ../tutorial/public
cp -r dist ../tutorial/public/doxygen

echo "Doxygen documentation updated successfully!"
echo "The documentation is now available at:"
echo "  - Direct access: ../doxygen/dist/index.html"
echo "  - Via tutorial: /doxygen (when VitePress is running)" 