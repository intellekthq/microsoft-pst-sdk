This page contains a quick overview of the NDB layer. For a more in depth description of what is contained, see the NDB module in the documentation distributed with the library. 
# NDB Layer Overview

The NDB Layer contains an implementation of all of the data structures defined in {{[MS-PST](MS-PST)}} 2.2, including abstractions for the database itself as well as blocks, pages (NBT/BBT), and nodes.

# [#Getting Started](#Getting-Started)
# [#Working with blocks and pages](#Working-with-blocks-and-pages)
# [#Iterating over the nodes of a database](#Iterating-over-the-nodes-of-a-database)
# [#Working with nodes](#Working-with-nodes)
## [#Reading from a node](#Reading-from-a-node)
## [#Opening a subnode](#Opening-a-subnode)

{anchor:Getting Started}
## Getting Started
A {{db_context}} object (usually referenced via a {{shared_ptr<db_context>}} typedef called {{shared_db_ptr}}) represents a PST file at the NDB layer. To create one, you call {{ open_database }} from database.h.

{{
#include "ndb.h" // includes all other ndb layer headers
...
shared_db_ptr pst_file = open_database(L"sample.pst");
}}
Think of a shared_db_ptr as an object factory which can create all of the other NDB object types. Its primary job is to abstract away all the differences between the different types of PST files and produce in memory abstractions of various structures on demand. All other database objects also keep a pointer to the {{db_context}} object. These other objects can be divided into two broad types, based on the type of reference to a {{db_context}} they keep.

**Independant Objects** Include the node class and every class in the LTP layer and above. These objects keep a strong reference (a proper {{shared_db_ptr}}) and thus extend the lifetime of the {{db_context}} to be at least that of their own.
**Dependant Objects** Consist of the entire {{block}} and {{page}} hierarchy. These objects only keep a weak reference (a {{weak_ptr<db_context>}} or {{weak_db_ptr}}) to the db_context, which does not extend the lifetime of the {{db_context}}.

{anchor:Working with blocks and pages}
## Working with Blocks and Pages
Blocks form the building blocks of nodes; pages form the building blocks of the NBT and BBT BTrees. Even when working with on the NDB layer, you will rarely interact with any of them except the root NBT page in order to do node lookups. 

To get the root NBT page, you simply call {{read_nbt_root()}} on your {{db_context}}:

{{
shared_ptr<nbt_page> nbtroot = pst_file->read_nbt_root();
}}
A {{nbt_page}} models the BTree concept described in [BTree Hierarchy](util_BTree), so can use it to lookup {{node_info}} objects, which is a simple structure mapping a node_id to its blocks, which can be used to construct a node. Or, you could use the {{db_context::lookup_node_info}} function, which does the exact same thing. Or, you could use {{db_context::lookup_node}}, which does the same thing, but constructs a {{node}} object from the {{node_info}} object. The following are equivalent:

{{
// Use the NBT root page
node_info mynode_info = nbtroot->lookup(0x31);
node mynode(pst_file, mynode_info);

// Use Lookup node info
node_info mynode_info2 = pst_file->lookup_node_info(0x31);
node mynode2(pst_file, mynode_info2);

// Use lookup node
node mynode3 = pst_file->lookup_node(0x31);
}}
All of this obvious abstracts away the details of working with the NBT pages; each NBT page knows how to read the appropriate child pages from the {{db_context}} using the various page factory functions. You can work with these page factory functions yourself; the basic idea is you provide the physical location of the page and the type and call the appropriate member function on {{db_context}} to read that page.

In a similar way, working with blocks is abstracted away by the {{node}} class. The {{node}} knows how to use the {{db_context}} to read the appropriate {{block}} type, and each {{block}} will know how to read any child blocks which may exist, etc. If you want to get very close to the metal you can use the block factory functions on {{db_context}}, you just provide basic information about the block and call the appropriate member function, and you'll get page an in memory abstraction for that block type.

{anchor:Iterating over the nodes of a database}
## Iterating over the nodes of a database
If you want to take a look at every node in the database, the NBT root page (via the BTree hierarchy) provides an iteration mechanism. Simply call {{nbt_page::begin}} to get an iterator positioned at the first {{node_info}} structure in the database, iterate until you hit the iterator returned by {{nbt_page::end}}, and do stuff in the middle. The basic form looks like this:

{{
void do_stuff(const node& n);
...

for(const_nodeinfo_iterator iter = nbt_root->begin(); iter != nbt_root->end(); ++iter)
{
    do_stuff(node(pst_file, *iter));
}
}}
Pretty straight forward. But what can you do with a {{node}}?

{anchor:Working with nodes}
## Working with nodes
The {{node}} class is the primary abstraction exposed by the NDB Layer. You can query information about it such as it's id, size, its block's id, etc; you can read information from it and treat it as a flat array of bytes (or a stream), and you can iterate over and open its subnodes.

Let's learn about our {{node}}:

{{
node_id id = n.get_id(); // returns the node_id, which can be used to lookup this node later
size_t size = n.get_size(); // returns the size of this node
node_id parent_id = n.get_parent_id(); // the parent id, as in a folder/message relationship. Don't confuse with node/subnode
}}
{anchor:Reading from a node}
### Reading from a node
Probably the most interesting thing to do with a {{node}} is read data from it. At the NDB Layer a node is just a flat array of bytes; higher level data structures are built on top of {{nodes}} by interpreting their content. But here, we can read from a node in one of two ways: directly, or by opening the node as a stream. To read from a node directly, you call one of the read overloads:

{{
std::vector<byte> content(15); // a vector of 15 bytes

 // read the first 15 bytes of the node
n.read(content, 0);

// read the first 15 bytes on the second "page" (here, means external block) of the node
n.read(content, 1, 0);

// use the read_raw function to not play with vectors
n.read_raw(&content[0](0), 15, 0);

// read<T> lets you read a arbitrary structure at an arbitrary location
disk::heap_first_header first_header = n.read<disk::heap_first_header>(0);
}}
The second way is to open the node as a stream. Then you can do any crazy thing you can do with a regular bidirectional/seekable istream object:

{{
node_stream nstream(n.open_as_stream());
float f;
node_stream >> f; // for some reason
}}
{anchor:Opening a subnode}
### Opening a subnode
A {{node}} may have zero or more subnodes. A subnode is in of itself a {{node}} (it can have subnodes itself, for example) - the only difference is that a subnode only exists in the context of a {{node}}, and does not exist in the top level NBT of the database.

Information about the subnodes is stored in a special block that each node can reference called a {{subnode_block}}. The {{subnode_block}} forms a BTree hierarchy, similar to the NBT, but private to that node. You can get a reference to the {{subnode_block}} and work with it directly, or you can call some convenience methods on the node to lookup or open various subnodes (similar to how you can get the nbt_root page from a {{db_context}}, or you can call convenience methods on {{db_context}} object itself). We'll focus on the convenience methods of {{node}}:

{{
for(const_subnodeinfo_iterator iter = n.subnode_info_begin(); iter != n.subnode_info_end(); ++iter)
{
    // construct a subnode and call our do_stuff function to work with it
    do_stuff(node(n, *iter));
}
}}
Also, {{node}} provides a pair of methods which return _proxy iterators_ over the subnodes. This basically means when you dereference the iterator, it returns an actual (temporary) node object constructed from the enclosing node and the subnode_info the iterator is positioned on. It just allows for some nicer looking and more natural syntax. The following is exactly equivalent to the above, and in fact does exactly what you see above behind the scenes:

{{
for(node::subnode_iterator iter = n.subnode_begin(); iter != n.subnode_end(); ++iter)
{
    // *iter is a node, no need to construct one ourselves
    do_stuff(*iter);
}
}}
And finally, {{node}} has a lookup method, for times you know exactly which subnode you're interested in.

{{
node mysubnode = n.lookup(0x2);
}}