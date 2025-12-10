#!/bin/bash

set -e

# Function to get version info from a file
get_version_info() {
	local file="$1"
	local major=$(grep "VERSION_MAJOR" "$file" | cut -d'=' -f2 | tr -d ' ')
	local minor=$(grep "VERSION_MINOR" "$file" | cut -d'=' -f2 | tr -d ' ')
	local patch=$(grep "PATCHLEVEL" "$file" | cut -d'=' -f2 | tr -d ' ')
	echo "$major.$minor.$patch"
}

# Function to update a VERSION file and return new version
update_version_file() {
	local file="$1"
	local major=$(grep "VERSION_MAJOR" "$file" | cut -d'=' -f2 | tr -d ' ')
	local current_minor=$(grep "VERSION_MINOR" "$file" | cut -d'=' -f2 | tr -d ' ')
	local current_patch=$(grep "PATCHLEVEL" "$file" | cut -d'=' -f2 | tr -d ' ')
	local new_minor=$((current_minor + 1))

	sed -i "s/VERSION_MINOR = $current_minor/VERSION_MINOR = $new_minor/" "$file"
	sed -i "s/PATCHLEVEL = $current_patch/PATCHLEVEL = 0/" "$file"

	echo "$major.$new_minor.0"
}

# Commit 1
new_version=$(update_version_file "VERSION")
git add VERSION
git commit -m "VERSION: update firmware version to $new_version

Update firmware version to $new_version"

# Commit 2
new_smc_version=$(update_version_file "app/smc/VERSION")
git add app/smc/VERSION
git commit -m "app: smc: VERSION: update firmware version to $new_smc_version

Update SMC version to $new_smc_version"

# Commit 3
new_dmc_version=$(update_version_file "app/dmc/VERSION")
git add app/dmc/VERSION
git commit -m "app: dmc: VERSION: update firmware version to $new_dmc_version

Update DMC version to $new_dmc_version"
