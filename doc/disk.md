This page contains a quick overview of the Disk layer. For a more in depth description of what is contained, see the Disk module in the documentation distributed with the library. 
# Disk Layer Overview

The Disk layer contains one file, disk.h. The file contains all of the structure definitions needed to interpret a PST file. Understanding what these structures are and what they are for is outside the scope of this intro; for that please refer to the PST file format specification. Each type is annotated with the section of {"[MS-PST](MS-PST)"} in which the data structure being defined is described.

Differences between ANSI and Unicode versions of PST files are handled via C{"++"} templates and template specializations when needed. For any given structure {{Foo}}, {{Foo<ulonglong>}} is the unicode PST version of that structure, and {{Foo<ulong>}} is the ANSI version of that structure.

To give one example, this is the definition of block_reference:

{{
template<typename T>
struct block_reference 
{
    typedef T block_id_disk;
    typedef T location;

    block_id_disk bid;
    location ib;
};
}}
A {{block_reference<ulonglong>}} is the Unicode PST 16 byte version of this structure, and a {{block_reference<ulong>}} is the ANSI PST 8 byte version. This works well as long as the difference between Unicode and ANSI stores is just the size of various elements in the structure. When the differences are more complex, template specialization is used:

{{
template<typename T>
struct page_trailer
{
};

template<>
struct page_trailer<ulonglong>
{
    typedef ulonglong block_id_disk;

    byte page_type;
    byte page_type_repeat;
    ushort signature;
    ulong crc;
    block_id_disk bid;
};

template<>
struct page_trailer<ulong>
{
    typedef ulong block_id_disk;

    byte page_type;
    byte page_type_repeat;
    ushort signature;
    block_id_disk bid;
    ulong crc;
};
}}
As you can see, the {{bid}} and {{crc}} members of {{page_trailer}} actually appear in a different order in Unicode and ANSI versions of the store. Thus, we need to specialize the template and explicitly layout the structures for each store type.

Alignment on all of these structures are very important. Yes, this is fragile, but in practice major compilers rarely change rules concerning structure packing. Eventually, static_asserts will be added to all structures to detect when compiling on a platform which isn't using the expected alignment.

It is never valid to instantiate any of these templates with any {{T}} other than {{ulonglong}} or {{ulong}}. 