//! \file
//! \brief Deleting items from a store, in place
//!
//! The caller picks what goes; this decides what that costs structurally. An
//! item's node is unlinked and everything only it owned is zeroed on disk, its
//! row comes out of the folder's table, and the folder's counts are corrected.
//!
//! These edits are made in place, so the store being edited is the store the
//! caller loses if something goes wrong. Work on a copy.
//!
//! Any node, table, folder or message object held across one of these calls is
//! stale afterwards and must be reopened.
//! \ingroup pst

#ifndef PSTSDK_PST_DELETE_H
#define PSTSDK_PST_DELETE_H

#include <set>
#include <vector>

#include "pstsdk/util/errors.h"
#include "pstsdk/util/primitives.h"

#include "pstsdk/ndb/writer.h"
#include "pstsdk/ltp/propbag.h"
#include "pstsdk/ltp/table.h"
#include "pstsdk/ltp/writer.h"

#include "pstsdk/mapitags.h"

namespace pstsdk
{

//! \cond write_api

//! \brief The \ref PR_MESSAGE_FLAGS bit marking a message read
//!
//! An unread message costs its folder a \ref PR_CONTENT_UNREAD as well as a
//! \ref PR_CONTENT_COUNT.
//! \sa [MS-OXCMSG] 2.2.1.6
const slong mapi_message_read = 0x1;

//! \brief Delete a message and everything hanging off it
//!
//! Takes the message's row out of its folder's table, corrects
//! \ref PR_CONTENT_COUNT and, when the message was unread,
//! \ref PR_CONTENT_UNREAD, then unlinks the node. Attachments, recipients and
//! embedded messages live in the message's subnode tree and go with it.
//! \throws key_not_found<node_id> if the message is not in the store
//! \param[in] db The store to edit, opened writable
//! \param[in] nid The message to delete
//! \ingroup pst_messagerelated
void delete_message(const shared_db_ptr& db, node_id nid);

//! \brief Delete one attachment, leaving the message in place
//!
//! Takes the attachment's row out of the message's attachment table, drops its
//! subnode and everything that hung off it including an embedded message, and
//! clears \ref PR_HASATTACH when it was the last one.
//! \throws key_not_found<node_id> if the message or attachment is not there
//! \param[in] db The store to edit, opened writable
//! \param[in] message_nid The message owning the attachment
//! \param[in] attachment_nid The attachment's subnode id
//! \ingroup pst_messagerelated
void delete_attachment(const shared_db_ptr& db, node_id message_nid, node_id attachment_nid);

//! \brief Delete a folder, its subfolders and everything in them
//!
//! Removes the folder's row from its parent's hierarchy table and clears
//! \ref PR_SUBFOLDERS when it was the last one, then deletes the folder's
//! property context and its contents, hierarchy and associated contents tables.
//! \throws key_not_found<node_id> if the folder is not in the store
//! \param[in] db The store to edit, opened writable
//! \param[in] nid The folder to delete
//! \ingroup pst_folderrelated
void delete_folder(const shared_db_ptr& db, node_id nid);

//! \brief Zero every byte the store does not reference
//!
//! Deleting an item clears what that item owned. This clears everything else the
//! store is not using, including text left in free space by whatever client
//! deleted things before the file reached you. Run it after the deletes.
//! \param[in] db The store to edit, opened writable
//! \returns The number of bytes zeroed
//! \ingroup pst
ulonglong wipe_free_space(const shared_db_ptr& db);

//! \endcond

} // end pstsdk namespace

//! \cond write_api
namespace pstsdk
{
namespace detail
{

//! An absent property leaves value untouched and reads as false
inline bool try_read_long(const shared_db_ptr& db, node_id nid, prop_id id, slong& value)
{
    try
    {
        property_bag bag(db->lookup_node(nid));
        if(!bag.prop_exists(id))
            return false;

        value = bag.read_prop<slong>(id);
        return true;
    }
    catch(key_not_found<node_id>&)
    {
        return false;
    }
}

//! The row ids of a table node, or nothing if the table is absent
inline std::vector<row_id> table_row_ids(const shared_db_ptr& db, node_id nid)
{
    std::vector<row_id> rows;

    try
    {
        table tc(db->lookup_node(nid));
        for(size_t i = 0; i < tc.size(); ++i)
            rows.push_back(tc[i].get_row_id());
    }
    catch(key_not_found<node_id>&) { }

    return rows;
}

inline std::vector<row_id> table_row_ids(const shared_db_ptr& db, node_id owner, node_id sub)
{
    std::vector<row_id> rows;

    table tc(db->lookup_node(owner).lookup(sub));
    for(size_t i = 0; i < tc.size(); ++i)
        rows.push_back(tc[i].get_row_id());

    return rows;
}

//! Every search folder's contents table, which caches copies of the columns it
//! indexes and so has to lose a deleted message's row along with the real folder.
//! The GUST is skipped: open_table refuses to parse it at all.
inline std::vector<node_id> search_contents_tables(const shared_db_ptr& db)
{
    std::vector<node_id> tables;
    std::shared_ptr<nbt_page> root = db->read_nbt_root();

    for(const_nodeinfo_iterator i = root->begin(); i != root->end(); ++i)
    {
        if(get_nid_type((*i).id) != nid_type_search_contents_table)
            continue;

        if((*i).id == nid_all_message_search_contents)
            continue;

        tables.push_back((*i).id);
    }

    return tables;
}

template<typename T>
inline void sweep_search_folders(db_writer<T>& writer, const std::vector<node_id>& tables,
                                 node_id message)
{
    for(size_t i = 0; i < tables.size(); ++i)
    {
        try { tc_remove_row(writer, tables[i], message); }
        catch(key_not_found<row_id>&) { }
    }
}

inline node_id folder_table(node_id folder, nid_type type)
{
    return make_nid(type, get_nid_index(folder));
}

//! Drop a row from whichever of a folder's two message tables holds it
template<typename T>
inline bool remove_message_row(db_writer<T>& writer, node_id folder, node_id message, bool& associated)
{
    try
    {
        tc_remove_row(writer, folder_table(folder, nid_type_contents_table), message);
        associated = false;
        return true;
    }
    catch(key_not_found<row_id>&) { }

    try
    {
        tc_remove_row(writer, folder_table(folder, nid_type_associated_contents_table), message);
        associated = true;
        return true;
    }
    catch(key_not_found<row_id>&) { }

    return false;
}

template<typename T>
inline void delete_message_impl(const std::shared_ptr<database_impl<T> >& db, node_id nid)
{
    const node_info info = db->lookup_node_info(nid);
    const node_id folder = info.parent_id;

    // Everything the counts depend on has to be read before the first write,
    // because a write invalidates the store's cached tree.
    slong flags = 0;
    const bool has_flags = try_read_long(db, nid, PR_MESSAGE_FLAGS, flags);
    const bool unread = has_flags && (flags & mapi_message_read) == 0;

    slong content_count = 0;
    slong unread_count = 0;
    slong associated_count = 0;
    const bool has_content = try_read_long(db, folder, PR_CONTENT_COUNT, content_count);
    const bool has_unread = try_read_long(db, folder, PR_CONTENT_UNREAD, unread_count);
    const bool has_associated = try_read_long(db, folder, PR_ASSOC_CONTENT_COUNT, associated_count);

    const std::vector<node_id> search = search_contents_tables(db);

    db_writer<T> writer(db);

    bool associated = false;
    const bool removed = folder != 0 && remove_message_row(writer, folder, nid, associated);

    if(removed && associated && has_associated && associated_count > 0)
        pc_set_inline(writer, folder, (prop_id)PR_ASSOC_CONTENT_COUNT, associated_count - 1);

    if(removed && !associated && has_content && content_count > 0)
        pc_set_inline(writer, folder, (prop_id)PR_CONTENT_COUNT, content_count - 1);

    if(removed && !associated && unread && has_unread && unread_count > 0)
        pc_set_inline(writer, folder, (prop_id)PR_CONTENT_UNREAD, unread_count - 1);

    sweep_search_folders(writer, search, nid);

    writer.delete_node(nid);
    writer.commit();
}

template<typename T>
inline void delete_attachment_impl(const std::shared_ptr<database_impl<T> >& db,
                                   node_id message_nid, node_id attachment_nid)
{
    db_writer<T> writer(db);
    const typename db_writer<T>::data_ref message = writer.node_ref(message_nid);

    if(message.sub == 0)
        throw key_not_found<node_id>(attachment_nid);

    const typename db_writer<T>::data_ref table =
        writer.subnode_ref(message.sub, nid_attachment_table);

    tc_remove_row(writer, table, attachment_nid);
    writer.subnode_remove(message.sub, attachment_nid);

    // the table survives with no rows, the way a message that never had an
    // attachment carries one, so the flag is what a client actually reads
    size_t left = 0;
    try { left = table_row_ids(db, message_nid, nid_attachment_table).size(); }
    catch(key_not_found<node_id>&) { }

    if(left == 0)
    {
        try { pc_set_inline(writer, message_nid, (prop_id)PR_HASATTACH, 0); }
        catch(key_not_found<prop_id>&) { }
    }

    writer.commit();
}

template<typename T>
inline ulonglong wipe_impl(const std::shared_ptr<database_impl<T> >& db)
{
    db_writer<T> writer(db);
    const ulonglong wiped = writer.wipe_free_space();
    writer.commit();
    return wiped;
}

template<typename T>
inline void delete_folder_contents(db_writer<T>& writer, const std::shared_ptr<database_impl<T> >& db,
                                   node_id folder, const std::vector<node_id>& search,
                                   std::set<node_id>& seen)
{
    // a hierarchy table naming an ancestor would otherwise recurse forever
    if(!seen.insert(folder).second)
        return;

    const std::vector<row_id> subfolders =
        table_row_ids(db, folder_table(folder, nid_type_hierarchy_table));

    for(size_t i = 0; i < subfolders.size(); ++i)
        delete_folder_contents(writer, db, subfolders[i], search, seen);

    // The tables go with the folder, so the messages only need their nodes
    // unlinked; there is no row left to take them out of.
    const std::vector<row_id> messages =
        table_row_ids(db, folder_table(folder, nid_type_contents_table));
    const std::vector<row_id> associated =
        table_row_ids(db, folder_table(folder, nid_type_associated_contents_table));

    for(size_t i = 0; i < messages.size(); ++i)
    {
        sweep_search_folders(writer, search, (node_id)messages[i]);
        try { writer.delete_node(messages[i]); }
        catch(key_not_found<node_id>&) { }
    }

    for(size_t i = 0; i < associated.size(); ++i)
    {
        try { writer.delete_node(associated[i]); }
        catch(key_not_found<node_id>&) { }
    }

    const nid_type tables[] = { nid_type_contents_table, nid_type_hierarchy_table,
                                nid_type_associated_contents_table };

    for(size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); ++i)
    {
        try { writer.delete_node(folder_table(folder, tables[i])); }
        catch(key_not_found<node_id>&) { }
    }

    try { writer.delete_node(folder); }
    catch(key_not_found<node_id>&) { }
}

template<typename T>
inline void delete_folder_impl(const std::shared_ptr<database_impl<T> >& db, node_id nid)
{
    const node_info info = db->lookup_node_info(nid);
    const node_id parent = info.parent_id;

    const size_t siblings =
        table_row_ids(db, folder_table(parent, nid_type_hierarchy_table)).size();
    const std::vector<node_id> search = search_contents_tables(db);

    db_writer<T> writer(db);

    if(parent != 0)
    {
        try { tc_remove_row(writer, folder_table(parent, nid_type_hierarchy_table), nid); }
        catch(key_not_found<row_id>&) { }

        // the expander in a client reads this flag rather than counting rows
        if(siblings == 1)
        {
            try { pc_set_inline(writer, parent, (prop_id)PR_SUBFOLDERS, 0); }
            catch(key_not_found<prop_id>&) { }
        }
    }

    std::set<node_id> seen;
    delete_folder_contents(writer, db, nid, search, seen);
    writer.commit();
}

} // end detail namespace
} // end pstsdk namespace

inline void pstsdk::delete_message(const shared_db_ptr& db, node_id nid)
{
    if(std::shared_ptr<large_pst> large = std::dynamic_pointer_cast<large_pst>(db))
        return detail::delete_message_impl(large, nid);
    if(std::shared_ptr<small_pst> small = std::dynamic_pointer_cast<small_pst>(db))
        return detail::delete_message_impl(small, nid);

    throw invalid_format();
}

inline void pstsdk::delete_attachment(const shared_db_ptr& db, node_id message_nid,
                                      node_id attachment_nid)
{
    if(std::shared_ptr<large_pst> large = std::dynamic_pointer_cast<large_pst>(db))
        return detail::delete_attachment_impl(large, message_nid, attachment_nid);
    if(std::shared_ptr<small_pst> small = std::dynamic_pointer_cast<small_pst>(db))
        return detail::delete_attachment_impl(small, message_nid, attachment_nid);

    throw invalid_format();
}

inline pstsdk::ulonglong pstsdk::wipe_free_space(const shared_db_ptr& db)
{
    if(std::shared_ptr<large_pst> large = std::dynamic_pointer_cast<large_pst>(db))
        return detail::wipe_impl(large);
    if(std::shared_ptr<small_pst> small = std::dynamic_pointer_cast<small_pst>(db))
        return detail::wipe_impl(small);

    throw invalid_format();
}

inline void pstsdk::delete_folder(const shared_db_ptr& db, node_id nid)
{
    if(std::shared_ptr<large_pst> large = std::dynamic_pointer_cast<large_pst>(db))
        return detail::delete_folder_impl(large, nid);
    if(std::shared_ptr<small_pst> small = std::dynamic_pointer_cast<small_pst>(db))
        return detail::delete_folder_impl(small, nid);

    throw invalid_format();
}
//! \endcond

#endif
