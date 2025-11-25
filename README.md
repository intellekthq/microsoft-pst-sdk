# pstsdk

pstsdk is a library for reading PST-format email archives.  It currently supports little-endian systems with modern C++ compilers. This fork continues from the `trunk` dir in the original CodePlex dump from the upstream repo.

**Important Links:**
- [Doxygen](https://intellekthq.github.io/microsoft-pst-sdk/)
- [MS-PST](doc/[MS-PST].pdf) - PST Spec v20100627
- Misc original docs/issues [/doc](doc)

This is a fork with the following changes:
- Build with `-std=c++17`
- `mapitags.h` [from WINE](https://gitlab.winehq.org/atticf/wine/-/blob/master/include/mapitags.h)

## Building

##### Windows

```
choco install cmake ninja make boost-msvc-14.3 -y
make
```

#### macOS

pstsdk can be built using the default clang distribution shipped with Xcode. Install
libiconv and boost from brew, then make.

```bash
brew install boost libiconv
make
```

#### Linux

Ubuntu

```bash
apt-get install -y build-essential cmake ninja-build libboost-all-dev
make
```

#### Running the unit tests

You can run the unit tests as follows:

    CTEST_OUTPUT_ON_FAILURE=1 make test

The unit tests should pass on all supported platforms.

## Credits

- Terry Mahaffey - Original author of pstsdk
- Jon Griffiths & WINE - mapitags.h header file

## License

This project is licensed under the Apache License 2.0. See the [LICENSE](LICENSE) file for details.

**Note:** `mapitags.h` is licensed under the [GNU Lesser General Public License v2.1+](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html).
