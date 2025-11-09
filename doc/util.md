This page contains a quick overview of the Util layer. For a more in depth description of what is contained, see the Util module in the documentation distributed with the library.
# Utilities Layer Overview

The utilities layer contains a couple concepts and a ton of type definitions used by other layers. It is not in and of itself a free standing layer; you won't ever work with just the Utilities layer. However, as the lowest layer on the stack, it is useful to know what is contained here as you'll certainly come across at least a few of these concepts when working at the higher layers. 

# [#BTree](#BTree)
# [#Exceptions](#Exceptions)
# [#Primitives](#Primitives)
## [#Basic Types](#Basic-Types)
## [#MAPI Types](#MAPI-Types)
# [#Utility Classes and Functions](#Utility-Classes-and-Functions)

{anchor:BTree}
## BTree Hierarchy
The concept of a "BTree" shows up three different times in the PST file format - the NBT and BBT BTrees which form the heart of the NDB, the SBLOCK BTrees to describe the subnodes of a node, and in the BTH structure (or BTree on Heap). It made sense to design a generic BTree class hierarchy so code concerning item lookup and iteration can be reused, simplifying the implementation and use of the various BTree instances. That generic BTree class hierarchy lives in the Util layer in Btree.h. It provides base classes for a BTree node, a leaf node, and a non-leaf node, as well as the basics of item lookup and relationship between those node classes. It also defines and implements an iteration mechanism. Users of this class just need to inherit and implement a few virtual functions for each node type to have a fully working BTree hierarchy.

The BTree Hierarchy is defined in {{pstsdk/util/btree.h}}.
{anchor:Exceptions}
## Exception Hierarchy
The various exception types (all derived directly or indirectly from std::exception) are defined in the util layer in {{pstsdk/util/errors.h}}.
{anchor:Primitives}
## Primitive Types
Some fundamental types used throughout the library are defined in {{pstsdk/util/primitives.h}}.
{anchor:Basic Types}
### Basic Types
Basic types include things like:

{{
typedef boost::uint32_t uint;
typedef boost::uint32_t ulong;
typedef boost::uint64_t ulonglong;
}}
Or

{{
typedef ulong node_id;
typedef ulonglong block_id;
typedef block_id page_id;
}}
{anchor:MAPI Types}
### MAPI Types
This includes types/enums specific to the PST as well as general MAPI definitions. Although the library tries to be as MAPI independant as possible, it's hard to avoid being aware of the different MAPI prop type definitions, or a few of the GUIDs defined for named properties.
{anchor:Utility Classes and Functions}
## Utility Classes and Functions
{{C++03}} lacks a portable way to address large files (>4GB), so the {{file}} class in {{pstsdk/util/util.h}} of the utilities layer was born.

Also located here are utility functions to convert between the differing time types.