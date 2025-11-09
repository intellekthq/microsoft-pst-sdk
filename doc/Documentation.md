# PST File Format SDK

PST File Format SDK is implemented in layers, which build a series of abstractions up from the structures on disk to higher level concepts like messages, folders, etc. You'll need to decide what level of abstraction you need, depending on the needs of your app. If you're writing a low level disk scraper to recover deleted or corrupt information from a PST file, you'll probably be best served by working with the disk layer directly (and good luck). If you're writing an application which just needs to extract high level information from the PST, you should jump in at the PST level, where the complexities of the file format are hidden.

Each layer maps to a subdirectory under the PST File Format SDK tree. By convention, each layer has a header file (_layer_.h) in the root of the project which includes all header files in that layer. Each layer is also only allowed to access itself and lower layers - never higher layers.

For most use cases, you'll only need to know the PST layer and a little bit about the LTP layer. Another set of use cases will only care about the Disk layer (if you're building your own API and just want to see the disk structures as a companion to the {{[MS-PST](MS-PST)}} documentation). Very few people will need to be familiar with the entire stack from top to bottom.

The documentation included in the PST File Format SDK distribution is the best API reference. Here is a quick 5 minute overview of each layer.

|| PST File Format SDK Architecture ||
| [PST Layer](pst) |
| [LTP Layer](ltp) |
| [NDB Layer](ndb) |
| [Disk Layer](disk) |
| [Utilities Layer](util) |