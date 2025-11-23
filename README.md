# pstsdk

pstsdk is a library for reading PST-format email archives.  It currently supports little-endian systems with modern C++ compilers.

This is a fork with the following changes:
- Build with `-std=c++17`
- `mapitags.h` [lifted from WINE](https://gitlab.winehq.org/atticf/wine/-/blob/master/include/mapitags.h)

## Building

##### Windows

TBD

#### macOS

pstsdk can be built using the default clang distribution shipped with Xcode. Install
libiconv and boost from brew, then make.

```bash
brew install boost libiconv
make
```

#### Linux

TBD, but same instructions as macOS.

#### Running the unit tests

You can run the unit tests as follows:

    CTEST_OUTPUT_ON_FAILURE=1 make test

The unit tests should pass on all supported platforms.
