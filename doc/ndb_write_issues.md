## NDB Write Issues

The list of work items here keeps growing. Currently on my mind:
* A "virtual_ref_tracker" class to handle refs in memory; so we can flush pending amap frees without worrying about deleting data from under an open node somewhere (although we still won't be able to flush pending amaps frees if there is an open context, anywhere, except the one requested the flush).
* Speaking of which, an amap abstraction is long overdue.
* db_contexts will be hierarchtical, you can open a context from inside a context. Commits "merge" the commting context into its parent. The "root context" is special, commiting to it hits the disk. You never have to explicitly commit the root context (...or do you? or should it happen on idle?)
* non-root contextx have nbt/bbt root pages saved at the position they were opened in, but they are never updated. instead they have seperate std::maps with "diffs" of the changes to the nodes and a std::map of the new blocks created, along with a std::vector of nodes deleted. db_context commits are going to be complicated...