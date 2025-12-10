#!/bin/bash
file="$1"
major=$(grep "VERSION_MAJOR" "$file" | cut -d'=' -f2 | tr -d ' ')
minor=$(grep "VERSION_MINOR" "$file" | cut -d'=' -f2 | tr -d ' ')
patch=$(grep "PATCHLEVEL" "$file" | cut -d'=' -f2 | tr -d ' ')
echo "$major.$minor.$patch"
