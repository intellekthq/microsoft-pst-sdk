# Big Picture Issues
* Write support. I'm currently working on building support, starting in the NDB level. The write support has to respect the immutability of blocks and pages at the NDB level, yet be intuitive at a higher level.
* GCC v4.4 support on other platforms
* Test suite expansion/rewrite. I want to use Boost.Unit, but I don't want to take a link dependancy on anything.
* Additional sample programs
* Test GCC v4.5 as a fully supported compiler on multiple platforms
* Test GCC v4.3 and v4.2 as partially supported compilers on multiple platforms