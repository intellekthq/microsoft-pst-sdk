It is my goal to eventually offer some level of support for all modern versions of Visual Studio and GCC.

### Fully Supported Compilers
Fully supported compilers are targeted explicitly by the SDK, by both the test suite and all sample programs. All fully supported compilers will compile the PST File Format SDK at the highest warning level with warnings treated as errors.
* Visual Studio 2010 RTM
* GCC v4.4, compiled with {{-std=c++0x}}
GCC v4.5 is being evaluated for eventual inclusion as a fully supported compiler.

### Partially Supported Compilers
Partially supported compilers are not explicitly targeted during development. Effort is spent getting these compilers to compile and pass the test suite before releases; but failure to do so may not necessarily hold up a release. Warnings may be generated on these platforms by either the SDK itself or Boost. Certain {{C++0x}} features are not available on these compilers, resulting in code which doesn't perform as well as on the fully supported compilers. You can suppress the {{"<C++ feature> not supported; consider updating your compiler"}} message by defining {{SUPPRESS_CPLUSPLUS0X_MESSAGES}} before including any SDK header.

Sample programs do not target and are not tested on these compilers. They probably work assuming the test suite does; but a sample program which uses a {{C++0x}} feature not supported by one of these compilers is not considered problematic.
* Visual Studio 2008 SP1. Must compile with {{/D_SCL_SECURE_NO_WARNINGS}} to suppress a warning in Boost.Iostreams.
* GCC v4.4 (without {{-std=c++0x}})
GCC v4.3 and v4.2 are being evaluated for eventual inclusion as partially supported compilers.

### Unsupported Compilers
Any compiler which does not implement the {{C++}} TR1 standard is not and will not ever be supported.