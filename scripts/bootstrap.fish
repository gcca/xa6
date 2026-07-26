#!/usr/bin/env fish

function fail
    echo "bootstrap: $argv" >&2
    exit 1
end

if test (uname -s) != Darwin
    fail "this bootstrap supports macOS only"
end

switch (uname -m)
    case arm64 x86_64
    case '*'
        fail "unsupported macOS architecture: "(uname -m)
end

set -l script_file (status --current-filename)
test -n "$script_file"
or fail "could not resolve the bootstrap script path"

set -l script_dir (path dirname (path resolve "$script_file"))
set -l repo_root (path dirname "$script_dir")
set -l manifest "$repo_root/build.ninja"
test -f "$manifest"
or fail "build.ninja was not found at $repo_root"

set -l linker_lines (string match -r '^ldlibs = .+$' <"$manifest")
test (count $linker_lines) -eq 1
or fail "expected one ldlibs declaration in build.ninja"

set -l linker_value (string replace -r '^ldlibs =\s*' '' -- $linker_lines[1])
set -l connector_formulae
for flag in (string split ' ' -- $linker_value)
    if string match -qr '^-l.+' -- $flag
        set -l library (string replace -r '^-l' '' -- $flag)
        if test "$library" != m
            set -a connector_formulae $library
        end
    end
end

test (count $connector_formulae) -eq 1
or fail "could not identify one connector formula from build.ninja"
set -l connector_formula $connector_formulae[1]
string match -qr '^[a-z0-9@+._-]+$' -- $connector_formula
or fail "build.ninja contains an invalid connector formula"

set -l formulae ninja googletest $connector_formula
set -l missing_formulae
for formula in $formulae
    if not brew list --formula $formula >/dev/null 2>&1
        set -a missing_formulae $formula
    end
end

if set -q missing_formulae[1]
    brew install $missing_formulae
    or fail "Homebrew could not install the required dependencies"
end

set -l ninja_prefix (brew --prefix ninja)
or fail "could not resolve the Ninja prefix"
set -l gtest_prefix (brew --prefix googletest)
or fail "could not resolve the GoogleTest prefix"
set -l connector_prefix (brew --prefix $connector_formula)
or fail "could not resolve the connector prefix"

set -l deps_dir "$repo_root/build/deps"
mkdir -p "$deps_dir"
or fail "could not create $deps_dir"

function link_prefix --argument-names target link
    if test -e "$link"; and not test -L "$link"
        fail "$link exists and is not a symbolic link"
    end
    ln -sfn "$target" "$link"
    or fail "could not link $link to its Homebrew prefix"
end

link_prefix "$gtest_prefix" "$deps_dir/gtest"
link_prefix "$connector_prefix" "$deps_dir/connector"

set -l ninja "$ninja_prefix/bin/ninja"
test -x "$ninja"
or fail "Ninja executable was not found at $ninja"

cd "$repo_root"
or fail "could not enter $repo_root"

rm -rf "$repo_root/build/dev" "$repo_root/build/dist"
or fail "could not clean generated build outputs"
$ninja build/dev/xa6
or fail "development compilation failed"
$ninja test
or fail "test compilation or execution failed"
$ninja dist
or fail "optimized compilation failed"

echo "bootstrap: build/dev/xa6 and build/dist/xa6 are ready"
