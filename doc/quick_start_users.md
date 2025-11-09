Using PST File Format SDK in your project is extremely simple.

# Unzip PST File Format SDK into a directory of your choosing (e.g, C:\libraries\pstsdk)
# Unzip [Boost](http://www.boost.org) v1.42 into a directory of your choosing (e.g, C:\libraries\boost)
# Make sure the PST File Format SDK directory is in your compilers include path
# Make sure the Boost directory is in your compilers include path

See the [Documentation](Documentation) page for a quick start guide.

Take a look at pstsdk/doc/html/index.html for an in depth API Reference.

Try to read {{[MS-PST](MS-PST)}}, the published file format documentation on the PST, if you're doing any serious work. It's also included in pstsdk/doc - I included it because various structures in the SDK are annotated with the section of MS-PST they describe/implement; and such sections numbers are likely to change in the future as the document is updated. The version of the document distributed with PST File Format SDK may be out of date, but the section numbers in it will correspond to the sections the API reference links to.

Be familiar with the [Compiler Support](Compiler-Support) offered by the PST File Format SDK.

