#!/bin/sh
# Host test of the NSP / XCI installer (SwitchPackage.java) on generated packages. Needs a JDK and python3 with
# the `cryptography` package.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../android/app/src/main/java/com/dsrnative/app/SwitchPackage.java"
tmp=$(mktemp -d)
python3 "$here/make_fixtures.py" "$tmp/fx"
javac -d "$tmp/classes" "$src" "$here/SwitchPackageTest.java"
java -cp "$tmp/classes" com.dsrnative.app.SwitchPackageTest "$tmp/fx"
rm -rf "$tmp"
