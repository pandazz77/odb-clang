# odb - ODB compiler

ODB is an open-source, cross-platform, and cross-database object-relational
mapping (ORM) system for C++. It allows you to persist C++ classes to a
relational database without having to deal with tables, columns, or SQL and
without manually writing any mapping code.

For further information, including licensing conditions, documentation, and
binary packages, refer to the [ODB project
page](https://codesynthesis.com/products/odb/).

This package contains the ODB compiler.

## Clang-based compiler

This version of the ODB compiler parses C++ with a built-in Clang frontend
instead of running as a GCC plugin. It is a single, self-contained
executable: the Clang/LLVM libraries are linked in statically and the Clang
builtin headers (`stddef.h`, etc) are embedded. No GCC installation is
required.

To parse your headers the compiler still needs a C++ standard library (the
headers only). By default it uses whatever the Clang driver detects for the
default target (for example, MinGW-w64 or the system GCC/libstdc++ or
libc++ headers on Linux). If no standard library headers are found for the
default target, then on Windows the latest installed Visual Studio (MSVC)
toolset is used automatically and on Linux the latest libstdc++ in
`/usr/include/c++/<version>` (which may be installed without GCC, for
example, `libstdc++-devel` on Fedora or `libstdc++-dev` on Alpine).
Compiler options can be passed with `-x`, for example:

```
odb -d sqlite -x --target=x86_64-pc-windows-msvc ...
odb -d pgsql  -x --sysroot=/opt/sysroot ...
```

The first `-x` value that doesn't start with `-` (a g++ executable name)
is ignored with a warning.

Building (requires static Clang/LLVM 22 libraries, CMake, and Ninja; run from
the repository root):

```
git submodule update --init
cmake -G Ninja -S odb -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

If `llvm-config` is not in `PATH`, pass `-DLLVM_CONFIG=<path>`.

On Linux the result is a fully static executable that runs on any x86-64
distribution (tested on Ubuntu 24.04, Debian 12, Fedora 41, and Alpine
3.20). See `etc/Dockerfile.linux` for a build environment.

Internally, the driver re-executes itself in the frontend mode (the hidden
`-cc1odb` option) for each input file and database, the same way it used
to run `g++ -fplugin=odb`.
