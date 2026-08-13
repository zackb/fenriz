#!/bin/bash

# fenriz Release Automation Script
# Usage: ./scripts/release.sh <version>
# Example: ./scripts/release.sh 0.1.0
#
# Releases the compositor and fenriz-desktop together under one tag: six assets
# (tar.gz + deb + rpm each) on one GitHub release, and four AUR packages.

set -e

VERSION=$1

if [ -z "$VERSION" ]; then
    echo "Usage: $0 <version> (e.g., 0.1.0)"
    exit 1
fi

TAG="v$VERSION"
REPO_ROOT=$(git rev-parse --show-toplevel)
ASSET_DIR="$REPO_ROOT/build/release"
DESKTOP_ASSET_DIR="$REPO_ROOT/desktop/build/release"

# Publishes packaging/aur/<name>/PKGBUILD into the sibling AUR checkout. pkgver and,
# for -bin packages, sha256sums are the only fields rewritten; the rest is verbatim.
# update_aur <pkgname> <pkgver> [tarball]
update_aur() {
    local name=$1 ver=$2 tarball=$3
    local dir="$REPO_ROOT/../$name"

    if [ ! -d "$dir" ]; then
        echo "⚠️  Warning: $dir not found, run packaging/aur/bootstrap.sh. Skipping."
        return
    fi

    cp "$REPO_ROOT/packaging/aur/$name/PKGBUILD" "$dir/PKGBUILD"
    sed -i "s/^pkgver=.*/pkgver=$ver/" "$dir/PKGBUILD"
    if [ -n "$tarball" ]; then
        sed -i "s/^sha256sums=.*/sha256sums=('$(sha256sum "$tarball" | cut -d' ' -f1)')/" "$dir/PKGBUILD"
    fi

    (
        cd "$dir"
        makepkg --printsrcinfo > .SRCINFO
        git add PKGBUILD .SRCINFO
        # index vs HEAD
        if git diff --cached --quiet; then
            echo "   $name already at $ver, nothing to push."
            exit 0
        fi
        git commit -m "update to $ver"
        git push origin master
        echo "   $name updated and pushed."
    )
}

# 1. Validation
for cmd in gh dpkg-deb rpmbuild; do
    if ! command -v "$cmd" &> /dev/null; then
        echo "Error: '$cmd' is not installed (needed to build or publish the release)."
        exit 1
    fi
done

if ! git diff-index --quiet HEAD --; then
    echo "Error: You have uncommitted changes. Please commit or stash them first."
    exit 1
fi

CURRENT_BRANCH=$(git branch --show-current)
if [ "$CURRENT_BRANCH" != "main" ]; then
    echo "Error: You are on branch '$CURRENT_BRANCH'. Releases must be performed from 'main'."
    exit 1
fi

echo "🚀 Starting release process for $TAG..."

# 2. Update both project versions (this is what CPack names the tarballs after)
sed -i "s/project(fenriz VERSION [0-9.]*/project(fenriz VERSION $VERSION/" "$REPO_ROOT/CMakeLists.txt"
sed -i "s/project(fenriz-desktop VERSION [0-9.]*/project(fenriz-desktop VERSION $VERSION/" "$REPO_ROOT/desktop/CMakeLists.txt"
git add "$REPO_ROOT/CMakeLists.txt" "$REPO_ROOT/desktop/CMakeLists.txt"
git commit -m "chore: bump version to $VERSION" || true

# 3. Tag and Push
echo "🏷️  Tagging $TAG..."
if git rev-parse "$TAG" >/dev/null 2>&1; then
    echo "Warning: Tag $TAG already exists locally."
else
    git tag -a "$TAG" -m "Release $TAG"
fi
git push origin main
git push origin "$TAG"

# 4. Build Packages
echo "📦 Building packages..."
rm -rf "$ASSET_DIR" "$DESKTOP_ASSET_DIR"
make package
make package-desktop

TARBALL="$ASSET_DIR/fenriz-$VERSION.tar.gz"
DESKTOP_TARBALL="$DESKTOP_ASSET_DIR/fenriz-desktop-$VERSION.tar.gz"

# 5. Create GitHub Release
echo "🌐 Creating GitHub Release..."
gh release create "$TAG" \
    "$TARBALL" \
    "$(ls "$ASSET_DIR"/fenriz-"$VERSION"*.deb | head -n 1)" \
    "$(ls "$ASSET_DIR"/fenriz-"$VERSION"*.rpm | head -n 1)" \
    "$DESKTOP_TARBALL" \
    "$(ls "$DESKTOP_ASSET_DIR"/fenriz-desktop-"$VERSION"*.deb | head -n 1)" \
    "$(ls "$DESKTOP_ASSET_DIR"/fenriz-desktop-"$VERSION"*.rpm | head -n 1)" \
    --title "Release $TAG" --generate-notes

# 6. Update AUR
echo "🧬 Updating AUR packages..."
update_aur fenriz-git "$VERSION"
update_aur fenriz-bin "$VERSION" "$TARBALL"
update_aur fenriz-desktop-git "$VERSION"
update_aur fenriz-desktop-bin "$VERSION" "$DESKTOP_TARBALL"

echo "✅ Full release $VERSION successfully deployed to GitHub and AUR!"
