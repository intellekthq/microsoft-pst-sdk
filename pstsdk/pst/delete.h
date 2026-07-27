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

//! \brief The \ref PR_MESSAGE_FLAGS bit that marks a message read
//!
//! Needed because an unread message costs its folder a count in
//! \ref PR_CONTENT_UNREAD as well as one in \ref PR_CONTENT_COUNT.
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

//! \endcond

} // end pstsdk namespace

//! \cond write_api
namespace pstsdk
{
namespace detail
{

//! Read a property that may not be there, without disturbing the store
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
    catch(key_not_found<node_id>&) { }

    try
    {
        tc_remove_row(writer, folder_table(folder, nid_type_associated_contents_table), message);
        associated = true;
        return true;
    }
    catch(key_not_found<row_id>&) { }
    catch(key_not_found<node_id>&) { }

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

    db_writer<T> writer(db);

    bool associated = false;
    const bool removed = folder != 0 && remove_message_row(writer, folder, nid, associated);

    if(removed && associated && has_associated && associated_count > 0)
        pc_set_inline(writer, folder, (prop_id)PR_ASSOC_CONTENT_COUNT, associated_count - 1);

    if(removed && !associated && has_content && content_count > 0)
        pc_set_inline(writer, folder, (prop_id)PR_CONTENT_COUNT, content_count - 1);

    if(removed && !associated && unread && has_unread && unread_count > 0)
        pc_set_inline(writer, folder, (prop_id)PR_CONTENT_UNREAD, unread_count - 1);

    writer.delete_node(nid);
    writer.commit();
}

template<typename T>
inline void delete_folder_contents(db_writer<T>& writer, const std::shared_ptr<database_impl<T> >& db,
                                   node_id folder)
{
    const std::vector<row_id> subfolders =
        table_row_ids(db, folder_table(folder, nid_type_hierarchy_table));

    for(size_t i = 0; i < subfolders.size(); ++i)
        delete_folder_contents(writer, db, subfolders[i]);

    // The tables go with the folder, so the messages only need their nodes
    // unlinked; there is no row left to take them out of.
    const std::vector<row_id> messages =
        table_row_ids(db, folder_table(folder, nid_type_contents_table));
    const std::vector<row_id> associated =
        table_row_ids(db, folder_table(folder, nid_type_associated_contents_table));

    for(size_t i = 0; i < messages.size(); ++i)
        writer.delete_node(messages[i]);

    for(size_t i = 0; i < associated.size(); ++i)
        writer.delete_node(associated[i]);

    const nid_type tables[] = { nid_type_contents_table, nid_type_hierarchy_table,
                                nid_type_associated_contents_table };

    for(size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); ++i)
    {
        try { writer.delete_node(folder_table(folder, tables[i])); }
        catch(key_not_found<node_id>&) { }
    }

    writer.delete_node(folder);
}

template<typename T>
inline void delete_folder_impl(const std::shared_ptr<database_impl<T> >& db, node_id nid)
{
    const node_info info = db->lookup_node_info(nid);
    const node_id parent = info.parent_id;

    const size_t siblings =
        table_row_ids(db, folder_table(parent, nid_type_hierarchy_table)).size();

    db_writer<T> writer(db);

    if(parent != 0)
    {
        try { tc_remove_row(writer, folder_table(parent, nid_type_hierarchy_table), nid); }
        catch(key_not_found<row_id>&) { }
        catch(key_not_found<node_id>&) { }

        // the expander in a client reads this flag rather than counting rows
        if(siblings == 1)
        {
            try { pc_set_inline(writer, parent, (prop_id)PR_SUBFOLDERS, 0); }
            catch(key_not_found<prop_id>&) { }
        }
    }

    delete_folder_contents(writer, db, nid);
    writer.commit();
}

//! Pick the store's format the way open_database does
template<typename Unicode, typename Ansi>
inline void dispatch(const shared_db_ptr& db, Unicode unicode, Ansi ansi)
{
    if(std::shared_ptr<large_pst> large = std::dynamic_pointer_cast<large_pst>(db))
    {
        unicode(large);
        return;
    }

    if(std::shared_ptr<small_pst> small = std::dynamic_pointer_cast<small_pst>(db))
    {
        ansi(small);
        return;
    }

    throw invalid_format();
}

struct delete_message_unicode
{
    node_id nid;
    void operator()(const std::shared_ptr<large_pst>& db) const { delete_message_impl(db, nid); }
};

struct delete_message_ansi
{
    node_id nid;
    void operator()(const std::shared_ptr<small_pst>& db) const { delete_message_impl(db, nid); }
};

struct delete_folder_unicode
{
    node_id nid;
    void operator()(const std::shared_ptr<large_pst>& db) const { delete_folder_impl(db, nid); }
};

struct delete_folder_ansi
{
    node_id nid;
    void operator()(const std::shared_ptr<small_pst>& db) const { delete_folder_impl(db, nid); }
};

} // end detail namespace
} // end pstsdk namespace

inline void pstsdk::delete_message(const shared_db_ptr& db, node_id nid)
{
    detail::delete_message_unicode unicode = { nid };
    detail::delete_message_ansi ansi = { nid };
    detail::dispatch(db, unicode, ansi);
}

inline void pstsdk::delete_folder(const shared_db_ptr& db, node_id nid)
{
    detail::delete_folder_unicode unicode = { nid };
    detail::delete_folder_ansi ansi = { nid };
    detail::dispatch(db, unicode, ansi);
}
//! \endcond

#endif
