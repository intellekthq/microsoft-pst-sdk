This page contains a quick overview of the LTP layer. For a more in depth description of what is contained, see the LTP module in the documentation distributed with the library. 
# LTP Layer Overview

The LTP Layer contains implementations of all of the data structures defined in {{[MS-PST](MS-PST)}} 2.3. These data structures are built on top of the {{node}} abstraction exposed by the [ndb](ndb).

# [#Getting Started](#Getting-Started)
# [#Coming up from the NDB](#Coming-up-from-the-NDB)
## [#Heaps](#Heaps)
## [#BTree-on-Heaps](#BTree-on-Heaps)
# [#Working with property objects](#Working-with-property-objects)
# [#Property Bags](#Property-Bags)
# [#Tables](#Tables)

{anchor:Getting Started}
## Getting Started
Unlike working with the [pst](pst) or [ndb](ndb), the LTP layer isn't free standing - there is no way to open some abstraction of a database in the LTP layer. So, in order to operate at this level, you are coming either from the PST layer where you "unwrapped" an object, or coming up from the NDB layer where you are interpreting nodes in a certain fashion. I'll assume you're doing the former, but we'll briefly go over the building blocks of the LTP first.

Similar to other layers; you can either include the rolled up ltp header or the individual headers,

{{
#include "ltp.h" // includes all other ltp layer headers
}}
{anchor:Coming up from the NDB}
## Coming up from the NDB
If you're coming up from the [ndb](ndb), we'll assume you have both a {{node}} object and a {{shared_db_ptr}} - at least, you can get one from the node. So what can we do to interpret a node at a higher level than just a flat stream of bytes? That is where the {{heap}} comes in.
{anchor:Heaps}
### Heaps
The first thing to know about a {{heap}} is that it completely controls a node. We say a given {{node}} _is a_ {{heap}}, **not** a {{heap}} _is in_ a {{node}}. You can not have two {{heap}} objects in a node. You can't have anything in a {{node}} which is a {{heap}} without the heap knowing about it, or you're risking really confusing someone. Think of a {{heap}} as a higher level interpretation of the entirety of a {{node}} - but not that {{node}} objects subnodes.

In general terms, almost all nodes in the NBT are organized as heaps. All message and folder nodes are. Certain internal queue nodes are not. The reverse is true for subnodes; most subnodes are not heaps but instead just overflow allocations which could not fit in the heap proper. However some subnodes are; such as attachment subnodes. In general, you just kind of have to "know" given the context of where the node is located, what its node type is, and who told you about it in order to be able to interpret the node properly.

The purpose of a {{heap}} is to subdivide a {{node}} into smaller allocations. A client can perform these operations on a {{heap}} in the PST:
* Allocate space and get back an allocation id. Max 3.8k in size.
* Read/write to an allocation id
* Free an allocation id
* Store a special allocation id called the "root" allocation
* Retrieve the "root" allocation id of a heap

Obviously since this is a read only library, we're limited to only doing half of the above.

Internally, a {{heap}} has very special and deep knowledge about the implementation of a {{node}}. That is to say, it is aware that a node's data is actually organized in blocks of 8k in size, potentially with an {{extended_block}} used to chain together multiple 8k {{external_block}} objects together to expose a logically larger area. The {{heap}} keeps book keeping information both at the start and end of each {{external_block}} in a node, keeping track of what space is allocated, etc. The heap allocation id's it gives out actually are a bit field which indicates which page (..external block) the allocation is on and the index into an allocation array kept at the end of that page, which gives the true offset of the allocation.

So that is the background information. To use a {{heap}} object is pretty simple; you construct one with a {{node}} you are wrapping or interpreting as a heap (this node will be copied unless you specify the alias constructor); then call various methods on it.

{{
node n = ....;
heap myheap(n);

heap_id root_allocation = myheap.get_root_id(); 
vector<byte> data = myheap.read(root_allocation); // hopefully you know what these bytes are
}}
You might be asking, "How do I know what the root allocation is, or points to?". Well, the first point is - the heap doesn't know, either. It just stores off the root allocation id for the client and returns it when asked. The person who does know is the _creator_ of the heap. And you should have enough context as to who the creator is, based on the node type, where you found the node, etc.
{anchor:BTree-on-Heaps}
### BTree-on-Heaps
The {{heap}} has one other special power; open a BTree-on-Heap object from within itself. A BTree-on-Heap (BTH) models the BTree hierarchy, just like the NBT/BBT. What it actually is a "header" allocation out of the heap which gives some information such as the key and value size, as well as the "root allocation" which is another allocation out of the heap which represents the root of the Tree structure. In reality, because heap allocations can be pretty large (3.8k), and the amount of data stored in a BTH is pretty small (a few dozen entries) it is pretty common for the "root allocation" to be a single leaf BTH node.

You may be familiar with working with BTree Hierarchy's by now. The idea is you get a pointer to the "root" node, and call lookup or iteration functions, and each node will know how to defer to child nodes if appropriate or do the lookup on itself if it is a leaf node.

Note that a BTH has no idea what data is stored in either the key or the value. It just blindly stores the pairs for interpretation by other structures.

Also note that a BTH is not to a {{heap}} what a {{heap}} is to a {{node}} - we say that a BTH is _inside of_ a {{heap}}. In reality a BTH is a collection of seperate but related {{heap}} allocations. You could have multiple unique BTH's in a {{heap}}, although in practice that isn't useful.

{{
unique_ptr<bth_node<prop_id, disk::prop_entry>> bth_root_node = myheap.open_bth(root_allocation);
disk::prop_entry foo = bth_root_node->lookup(0x3001);
}}
Generally, you will not work directly with a BTH object, but rather use other LTP objects which use them.
{anchor:Working with property objects}
## Working with property objects
Before we get into the specific LTP objects built on top of a {{heap}} and a {{BTH}}, we should discuss what a property object is. A property object, or the {{const_property_object}} base class, is both a base class and an interface for accessing "properties" on an "object". There are two implementations we'll discuss shortly - the {{property_bag}} and {{const_table_row}}. But for now, a {{const_property_object}} is any object you can enumerate the properties of and read those properties.

The iterface is simple; you call {{const_property_object::read_prop<T>(prop_id)}} to read a given property (there is also an array version for multivalued properties - as well as varients for opening a property as a stream). You choose how to interpret the property (you supply the "T"). If you don't know, you can call {{const_property_object::get_prop_type(prop_id)}}. You can also call {{const_property_object::get_prop_list}} to read all properties on the object.

The following code fragment iterates over all properties on a given property object, and does something with all of the string types:

{{
const_property_object& obj = ...;
vector<prop_id> prop_list = obj.get_prop_list();

for(vector<prop_id>::iterator iter = prop_list.begin(); iter != prop_list.end(); ++iter)
{
    if(obj.get_prop_type(*iter) == prop_type_wstring)
    {
        wstring prop_val = obj.read_prop<wstring>(*iter);
        // do something with prop_val
    }
}
}}
There are methods to check to see if a property exists as well. You can also open any variable length property as a stream, and do something special with it. This is useful for large properties you may not want to load in memory all at once.

{{
if(obj.prop_exists(0x1000))
{
    prop_stream body(obj.open_prop_stream(0x1000));
    // do stream type stuff with body
}
}}
{anchor:Property Bags}
## Property Bags
A {{property_bag}} is the implementation of what {{[MS-PST](MS-PST)}} calls a Property Context or PC. On disk, it is a heap (which is a node), which has a single BTH stored in it, the root of which stored as the root allocation of the heap. The key of the BTH is the prop_id, and the value is something called a {{disk::prop_entry}}. The prop entry gives you the type of the property as well as where it is located - its storage is defined by the type and the allocation id. It could be inline, or it could be a heap allocation, or it could be a subnode.

All of this logic is encapsulated in the {{property_bag}}. You'll be most familiar with this class because this is what you get back when you "unwrap" a [pst](pst) layer object. For example, if you want to read some property off of a {{message}} not directly exposed, such as PidTagCreationTime:

{{
message m = ...;
property_bag& bag = m.get_property_bag(); // unwrap down to the LTP
time_t creation_time = bag.read_prop<time_t>(0x3007);
... // gloat about knowing the creation time of this message
}}
There is not much else to {{property_bag}} besides its {{const_property_object}} implementation except perhaps its ability to unwrap down to the {{node}} itself, if needed.
{anchor:Tables}
## Tables
A {{table}} is the implementation of what {{[MS-PST](MS-PST)}} calls a Table Context or TC. It is a heap (which is a node) which has a "table header" stored as the root allocation. The table header gives information about the table such as the number and type of columns, the root of a BTH which maps row identifiers (usually node_ids) to row offsets, and the row matrix allocation. The row matrix allocation is either a heap allocation itself (not usually, only for very tiny tables) or a subnode. 

The table context is the most complicated structure in the PST (narrowly beating out the {{name_id_map}} structure, not discussed here). Fortunately, all of this logic is encapsulated in an easy to use {{table}} class. You will probably be familiar with the class because this is what you get back when you "unwrap" a [pst](pst) {{folder}} object to its contents table (messages in the folder) or hierarchy table (subfolders).

A {{table}} allows you to access its rows. A row is basically a {{const_property_object}} giving the properties of that row. It's that simple. The easiest way to access the rows is by using operator[]():

{{
table& tab = ...;
for(int i = 0; i < tab.size(); ++i)
{
    const_table_row row = tab[i](i);
    ... // party on row
}
}}
Iterator style access (begin/end, etc) is also supported.

The one other capability the table provides is fast lookup by {{row_id}}. The exact {{row_id}} type varies by table, but for contents tables and hierarchy tables it is the {{node_id}} of the object. So if you know the {{node_id}}, you can do this for example:

{{
node_id mysubfolder = ...; // I know this somehow
ulong row = tab.lookup_row(mysubfolder);
wstring displayname = tab[row](row).read_prop<wstring>(0x3001);
}}
Under the hood this is a BTH lookup, into a BTH maintained for this exact purpose.