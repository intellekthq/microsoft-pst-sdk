# Win32
# Install Visual Studio 2010
# Install [SVN](http://www.collab.net/downloads/subversion) to C:\svn, [learn how to use it](http://www.bing.com/search?q=svn+tutorial&form=QBLH&scope=web&qs=n)
# Install [MinGW Make](http://www.mingw.org/) to C:\mingw. I renamed C:\mingw\bin\mingw32-make.exe to just make.exe for sanity's sake
# Check out the current trunk to C:\libraries\fairport
# Install (unzip) [Boost](http://www.boost.org) to C:\libraries\boost
# Create a batch file which:
## Executes the "Visual Studio 2010 Command Prompt" batch file
## Adds C:\svn to %PATH%
## Adds C:\mingw\bin to %PATH%
## Adds C:\libraries\fairport and C:\libraries\boost to %INCLUDE%
## Drops you into C:\libraries\fairport
# Try getting the "test suite" to compile (C:\libraries\fairport\test) by running make

Once you're setup and building, you should take a look at the [Big Picture Issues](big_issues) to see areas where you can help out.