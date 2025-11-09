This page contains a quick overview of the PST layer. For a more in depth description of what is contained, see the PST module in the documentation distributed with the library. 
# PST Layer Overview

If you're confused and trying to figure out how to use this library to just get an email out of a PST, congratulations - you found the right page. The PST layer provides the highest level of abstraction, with simple to use objects like a {{pst}}, {{folder}}, {{message}}, etc. Each object provides friendly wrappers around common properties, provides access to "child" objects, and allows you to even unwrap to the LTP layer if you need to do any complex processing. Here we'll cover the basics of how to open a PST file, access some folders, access some messages, and access some properties on those messages.

# [#Getting Started](#Getting-Started)
# [#The PST Object](#The-PST-Object)
## [#Accessing Folders](#Accessing-Folders)
# [#Working with Folders](#Working-with-Folders)
## [#Accessing Subfolders](#Accessing-Subfolders)
## [#Accessing Messages](#Accessing-Messages)
# [#Working with Messages](#Working-with-Messages)
## [#The content of messages](#The-content-of-messages)
## [#Accessing Attachments](#Accessing-Attachments)
## [#Accessing Recipients](#Accessing-Recipients)
# [#Working with Attachments](#Working-with-Attachments)
# [#Working with Recipients](#Working-with-Recipients)
# [#Unwrapping to the LTP](#Unwrapping-to-the-LTP)

{anchor:Getting Started}
## Getting Started
A {{pst}} object represents both a PST and OST file, both ANSI and Unicode versions. It is the entry point to working on the PST Layer. To get started, you need to create an instance of a {{pst}} object

{{
#include "pst.h" // includes all pst layer headers
...
pst myfile(L"myfile.pst");
}}
{anchor:The PST Object}
## The PST Object
The {{pst}} object does a lot of things - it has properties on it you can query, it supports iteration over every {{message}} in the store, every {{folder}} in the store, it also exposes named property mapping. But the most interesting things here for a quick start guide involve getting past the {{pst}} object and getting ahold of a {{folder}} object so we can eventually get to the {{message}} objects it contains. So let's focus on that, and move on.

{anchor:Accessing Folders}
### Accessing Folders
There are three basic methods to access any given folder from a {{pst}} object:
# Iterate over every folder in the store using the provided iterators, and stop when you found the right folder
# Open the root folder, and recursively look at its sub folders
# Open the folder by name

The {{pst}} object provides a pair of iterators, given by {{pst::folder_begin}} and {{pst::folder_end}}, which you can use to enumerate all of the folders in the store. There is a similar pair for all the messages in the store. The folders will be iterated over in a seemingly random order (by node id, which turns out to basically be by creation time) regardless of the actual underlying hierarchy. This is what it looks like:

{{
void do_stuff(const folder& f);
...
for(pst::folder_iterator iter = myfile.folder_begin(); iter != myfile.folder_end(); ++iter)
{
    do_stuff(*iter);
}
}}
The actual iterator type is kind of complicated. It's a proxy iterator, so it is constructing a temporary {{folder}} object when dereferenced from the actual iterator type; which is really a transform iterator over a filter iterator over a {{node_info}} iterator from the NDB layer. But don't worry about that.

So that's one way; you can then implement do_stuff as appropriate. You can also open the root folder, and perhaps use that to look at subfolders, and recurse down the folder tree as appropriate:

{{
folder fold = myfile.open_root_folder();
.... // party with fold, perhaps open other subfolders
}}
One final option is to use a feature to open a folder by name:

{{
folder inbox = myfile.open_folder(L"Inbox");
... // party with the inbox
}}
This method actually uses the first; it just stops the iteration and returns the first folder which matches the supplied display name. Simple but convenient.
{anchor:Working with Folders}
## Working with Folders
So, once you have a folder, you can do three things really:
# Access properties on the folder
# Access subfolders of the folder
# Access the messages in this folder

Accessing properties on the folder is pretty straight forward. There are a bunch of methods on the folder object which tell you about the folder. Accessing subfolders or the messages in the folder involves working with a pair of iterators.
{anchor:Accessing Subfolders}
### Accessing Subfolders
The {{folder}} object provides a pair of iterators, {{folder::sub_folder_begin}} and {{folder::sub_folder_end}}, which allow access to the sub folders of that {{folder}}.

{{
for(folder::folder_iterator iter = fold.sub_folder_begin(); iter != fold.sub_folder_end(); ++iter)
{
    do_stuff(**iter); // **iter is one subfolder of this folder
}
}}
You can also lookup a subfolder by name. This is not a recursive operation - it only looks at folders which are direct children of this folder.

{{
folder leah = fold.open_sub_folder(L"Pictures of my Daughter");
}}
She's really cute.
{anchor:Accessing Messages}
### Accessing Messages
The {{folder}} object also provides a pair of iterators, {{folder::message_begin}} and {{folder::message_end}}, which allow access to messages contained in that {{folder}}.

{{
void do_stuff(const message& m); 
...
for(folder::message_iterator iter = fold.message_begin(); iter != fold.message_end(); ++iter)
{
    do_stuff(**iter); // **iter is a message in this folder
}
}}
There is no equivalent lookup of a message by name. Sorry.
{anchor:Working with Messages}
## Working with Messages
Congratulations, you have a message object! This object allows access to the data you actually care about. Note that every type of object you can think of is a {{message}} in the PST, this includes not just mail items but calendar items, task items, contacts, etc. They're all {{message}} objects with different sets of properties on them.

The basic {{message}} abstraction doesn't care about all of that, however. It just provides access to the most commonly requested properties as well as access through iterators to the attachments and recipients of the message.

{anchor:The content of messages}
### The content of messages
The {{message}} object has methods to get the subject, body (both html and plain text, both as a blob and a stream), as well as the attachment and recipient count. It looks something like this:

{{
message m = ...
wstring subject = m.get_subject();
wstring body = m.get_body();
wstring htmlbody = m.get_html_body();

prop_stream bodystream(m.open_body_stream());
prop_stream htmlbodystream(m.open_html_body_stream());

size_t num_attachments = m.get_attachment_count();
size_t num_recipients = m.get_recipient_count();
}}
Pretty straight forward. Congratulations, you've read a message from a PST!

{anchor:Accessing Attachments}
### Accessing Attachments
You access the attachments of a {{message}} using the {{message::attachment_begin}} and {{message::attachment_end}} functions, which return the standard begin/end iterator pairs over the message attachment collection. You should check to ensure that {{message::get_attachment_count}} is greater than 0 before calling these methods.

{{
void do_stuff(const attachment& attach);
...
if(m.get_attachment_count() > 0)
{
    for(message::attachment_iterator iter = m.attachment_begin(); iter != m.attachment_end(); ++iter)
    {
        do_stuff(**iter); // **iter is an attachment on this message
    }
}
}}
{anchor:Accessing Recipients}
### Accessing Recipients
Pretty much identical to accessing attachments, except call {{message::recipient_begin}} and {{message::recipient_end}}.

{{
void do_stuff(const recipient& r);
...
for(message::recipient_iterator iter = m.recipient_begin(); iter != m.recipient_end(); ++iter)
{
    do_stuff(**iter); // **iter is a recipient on this message
}
}}
{anchor:Working with Attachments}
## Working with Attachments
Ok, you have an attachment. Now what? Well, the three most important methods to know are these:

{{
attachment attach = ...;
wstring filename = attach.get_filename();
vector<byte> content = attach.get_bytes();
prop_stream content_stream(attach.open_byte_stream());
}}
It is left as an exercise to the reader to decode what these mysterious methods do.

{{operator<<}} for working with ostreams is provided, so implementing a "save to disk" function is actually pretty trivial:

{{
void save_to_disk(const attachment& attach)
{
    std::wstring wfilename = attach.get_filename();
    // stupid ofstream wanting ansi C strings
    std::string filename(wfilename.begin(), wfilename.end()); 

    ofstream newfile(filename.c_str(), ios::out | ios::binary);
    newfile << attach;    
}
}}
And one last thing: some attachments are actually themselves messages. These are sometimes called "embedded messages". This is explicitly supported by the {{attachment}} object, allowing you to convert any {{attachment}} which is really an embedded message into a proper {{message}} object. Two methods help here, {{attachment::is_message}} and {{attachment::open_as_message}}.

{{
if(attach.is_message())
{
    message m = attach.open_as_message(); // throws bad_cast if is_message() == false
    do_stuff(m);
}
}}
That's it for attachments.
{anchor:Working with Recipients}
## Working with Recipients
A {{recipient}} is a much smpler object than an {{attachment}}, it merely provides access to some simple properties. Here are some examples:

{{
recipient r = ...;
wstring name = r.get_name(); // the name of the recipient
wstring email = r.get_email_address(); // the email address of the recipient
recipient_type t = r.get_type(); // get the type of the recipient, to, cc, etc.
}}
{anchor:Unwrapping to the LTP}
## Unwrapping to the LTP
As a final note; all of the above objects: the {{pst}}, {{folder}}, {{message}}, {{recipient}}, and {{attachment}} - all provide access to the underlying LTP objects. This means that if you're not happy with the properties exposed by the {{message}}, you can call {{message::get_property_bag}} and access the other properties directly. If you need to do some advanced table sorting, or merely want to be more efficient with your folder or message iteration, you can use {{folder::get_contents_table}} or {{folder::get_hierarchy_table}} to get the LTP {{table}} objects backing those structures. Even the {{pst}} object allows you to grab it's {{property_bag}}, the {{name_id_map}} for the store, or even the lowest level NDB {{shared_db_ptr}}. A master of the PST layer is at minimum very comfortable at the LTP layer and aware of the NDB Layer and its constructs.