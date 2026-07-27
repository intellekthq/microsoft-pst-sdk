//! \file
//! \brief In place edits of an open store
//!
//! Every edit here writes back to the address it read from, at the same size or
//! smaller. Nothing is relocated, no block or page is ever created, and the
//! space that falls out is recovered by the client's AMap rebuild rather than
//! by us.
//!
//! A block with more than one reference can be unlinked but never modified,
//! because there is nowhere to copy it to first.
//! \ingroup ndb

#ifndef PSTSDK_NDB_WRITER_H
#define PSTSDK_NDB_WRITER_H

#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

#include "pstsdk/util/errors.h"
#include "pstsdk/util/primitives.h"

#include "pstsdk/ndb/database.h"

namespace pstsdk
{

//! \cond write_api

//! \brief Performs in place edits on an open store
//!
//! Holds no state of its own beyond a dirty flag; the store's header lives in
//! the \ref database_impl and is flushed by \ref commit.
//! \tparam T ulonglong for a Unicode store, ulong for an ANSI store
//! \ingroup ndb_databaserelated
template<typename T>
class db_writer
{
public:
    explicit db_writer(const std::shared_ptr<database_impl<T> >& db)
        : m_db(db), m_dirty(false) { }

    //! \brief Read a block's payload, decoded
    //! \param[in] bid The block to read
    //! \returns Exactly the block's unaligned size in bytes
    std::vector<byte> read_block(block_id bid);

    //! \brief Write a payload back over the block it came from
    //!
    //! Encodes, stamps a fresh trailer at the position the new size implies, and
    //! zeroes the old extent first when the block shrinks past a slot boundary.
    //! \throws can_not_resize if the payload is larger than the block
    //! \throws shared_block if the block has more than one reference
    //! \param[in] bid The block to overwrite
    //! \param[in] payload The new contents, at most the block's current size
    void write_block(block_id bid, const std::vector<byte>& payload);

    //! \brief Remove a node's entry from the NBT
    //! \throws key_not_found<node_id> if the node is not in the tree
    //! \param[in] nid The node to unlink
    void nbt_remove(node_id nid);

    //! \brief Unlink a node and scrub everything only it owned
    //!
    //! Both trees are collected first, because neither is reachable once the NBT
    //! entry is gone, then unlinked before scrubbing so the store stays
    //! structurally valid throughout.
    //! \throws key_not_found<node_id> if the node is not in the tree
    //! \param[in] nid The node to delete
    void delete_node(node_id nid);

    //! \brief Where a node or subnode keeps its data and its own subnodes
    //!
    //! Subnodes carry both, which is how a message's attachment table has a heap
    //! of its own and a row matrix hanging off it.
    struct data_ref
    {
        block_id data;
        block_id sub;
    };

    //! \brief The pair belonging to a top level node
    //! \throws key_not_found<node_id> if the node is not in the store
    data_ref node_ref(node_id nid);

    //! \brief The pair belonging to a subnode of a given subnode tree
    //! \throws key_not_found<node_id> if the tree has no such subnode
    //! \param[in] tree The root of the subnode tree to search
    //! \param[in] sub The subnode to find
    data_ref subnode_ref(block_id tree, node_id sub);

    //! \brief Drop a subnode from a subnode tree and release what it owned
    //!
    //! The entry count governs, so the block keeps its size and only loses the
    //! entry. Empty leaves are taken out of their parent the same way.
    //! \throws key_not_found<node_id> if the tree has no such subnode
    //! \param[in] tree The root of the subnode tree
    //! \param[in] sub The subnode to remove
    //! \returns true when the tree has no subnodes left
    bool subnode_remove(block_id tree, node_id sub);

    //! \brief The external blocks of a data tree, in logical order
    //! \param[in] bid The root of the tree
    std::vector<block_id> external_blocks(block_id bid);

    //! \brief Shrink the last external block of a data tree
    //!
    //! Passing zero drops the block instead, taking it out of its parent and
    //! releasing it, which is how a row matrix gives back a whole page. The
    //! parent's total_size is kept in step because
    //! \ref extended_block::get_page_count derives the page count from it and
    //! asserts the result against the real child count.
    //! \param[in] bid The root of the tree
    //! \param[in] new_size The last block's new size, or zero to drop it
    //! \returns true when the tree has no blocks left
    bool shrink_data_tail(block_id bid, size_t new_size);


    //! \brief Zero every byte the store does not reference
    //!
    //! What survives is the header region, every page that validates as one, and
    //! every block the BBT lists. BTree pages are the trap: they come from
    //! bidNextP and never appear in the BBT, so anything treating "outside the
    //! BBT" as free destroys the store.
    //! \returns The number of bytes zeroed
    ulonglong wipe_free_space();


    //! \brief Flush the header and drop the store's cached BTree roots
    //!
    //! Invalidates the AMaps so the client rebuilds them, which is what actually
    //! returns the freed slots. Does nothing if no edit has been made.
    void commit();

private:
    void zero_extent(ulonglong address, size_t size);
    void bbt_remove(block_id bid);
    void bbt_set_size(block_id bid, ushort cb);
    void bbt_set_ref_count(block_id bid, ushort count);
    //! A block someone else still owns keeps its bytes and only loses a reference
    void release_block(block_id bid);

    //! Offset of a BTree page's four metadata bytes: count, max, entry size, level
    static const size_t bt_meta = disk::page<T>::page_data_size - sizeof(T);

    static uint bt_count(const std::vector<byte>& page) { return page[bt_meta]; }
    static uint bt_entry_size(const std::vector<byte>& page) { return page[bt_meta + 2]; }
    static uint bt_level(const std::vector<byte>& page) { return page[bt_meta + 3]; }

    //! The key is the first field of every entry type, leaf and nonleaf alike
    static T bt_key(const std::vector<byte>& page, uint index);
    static void bt_set_key(std::vector<byte>& page, uint index, T key);
    //! A nonleaf entry is a key then a block_reference, so the address trails both
    static ulonglong bt_child(const std::vector<byte>& page, uint index);
    static int bt_search(const std::vector<byte>& page, T key);

    std::vector<byte> read_page_raw(ulonglong address);
    void write_page_raw(ulonglong address, std::vector<byte>& page);

    void bt_descend(ulonglong root, T key, std::vector<ulonglong>& path, std::vector<uint>& indices);
    void bt_remove(ulonglong root, T key);
    //! Retarget the ancestors that named a page by its old first key
    void bt_propagate_key(const std::vector<ulonglong>& path, const std::vector<uint>& indices,
                          size_t depth, T key);
    //! Locate a leaf entry so a caller can patch its non-key fields in place
    void bt_find(ulonglong root, T key, ulonglong& address, uint& index);

    void collect_data_tree(block_id bid, std::vector<block_id>& blocks);
    void collect_subnode_tree(block_id bid, std::vector<block_id>& blocks);

    void block_children(block_id bid, std::vector<block_id>& children);

    //! Work out what dropping a reference to a tree costs, without changing anything
    void plan_release(block_id root, std::map<block_id, ushort>& remaining,
                      std::map<block_id, block_info>& info, std::vector<block_id>& order);
    //! Carry out a plan: rewrite the counts that dropped, unlink and scrub the rest
    void apply_release(const std::map<block_id, ushort>& remaining,
                       const std::map<block_id, block_info>& info,
                       const std::vector<block_id>& order);

    void collect_pages(ulonglong address, std::vector<ulonglong>& pages);
    bool looks_like_page(ulonglong address);

    ulonglong nbt_root() const { return m_db->get_header().root_info.brefNBT.ib; }
    ulonglong bbt_root() const { return m_db->get_header().root_info.brefBBT.ib; }

    void stamp_header_crc();

    std::shared_ptr<database_impl<T> > m_db;
    bool m_dirty;
};

//! \endcond

} // end pstsdk namespace

//! \cond write_api
template<typename T>
inline std::vector<pstsdk::byte> pstsdk::db_writer<T>::read_block(block_id bid)
{
    block_info bi = m_db->lookup_block_info(bid);
    std::vector<byte> buffer = m_db->read_block_data(bi);
    buffer.resize(bi.size);

    if(bi.size == 0 || !disk::bid_is_external(bi.id))
        return buffer;

    if(m_db->get_header().bCryptMethod == disk::crypt_method_permute)
        disk::permute(&buffer[0], bi.size, false);
    else if(m_db->get_header().bCryptMethod == disk::crypt_method_cyclic)
        disk::cyclic(&buffer[0], bi.size, (ulong)bi.id);

    return buffer;
}

template<typename T>
inline void pstsdk::db_writer<T>::write_block(block_id bid, const std::vector<byte>& payload)
{
    block_info bi = m_db->lookup_block_info(bid);

    if(payload.size() > bi.size)
        throw can_not_resize("write_block cannot grow a block");

    if(bi.ref_count > disk::block_unreferenced + 1)
        throw shared_block("cannot edit a block in place while it is shared");

    const size_t new_cb = payload.size();
    const size_t old_aligned = disk::align_disk<T>(bi.size);
    const size_t new_aligned = disk::align_disk<T>(new_cb);

    std::vector<byte> buffer(new_aligned, 0);
    if(new_cb > 0)
    {
        memcpy(&buffer[0], &payload[0], new_cb);

        // extended and subnode blocks are stored plain; only external blocks
        // carry the store's encoding
        if(disk::bid_is_external(bi.id))
        {
            if(m_db->get_header().bCryptMethod == disk::crypt_method_permute)
                disk::permute(&buffer[0], (ulong)new_cb, true);
            else if(m_db->get_header().bCryptMethod == disk::crypt_method_cyclic)
                disk::cyclic(&buffer[0], (ulong)new_cb, (ulong)bi.id);
        }
    }

    disk::block_trailer<T>* bt = reinterpret_cast<disk::block_trailer<T>*>(
        &buffer[0] + new_aligned - sizeof(disk::block_trailer<T>));
    bt->cb = (ushort)new_cb;
    bt->signature = disk::compute_signature(bi.id, bi.address);
    bt->bid = bi.id;
    // over the encoded bytes: read_block_data checks the CRC before
    // read_external_block gets a chance to decode
    bt->crc = disk::compute_crc(&buffer[0], (ulong)new_cb);

    // a shrink past a slot boundary moves the trailer, so the old one has to go
    if(new_aligned < old_aligned)
        zero_extent(bi.address, old_aligned);

    m_db->get_file().write(buffer, bi.address);
    m_dirty = true;

    if(new_cb != bi.size)
        bbt_set_size(bid, (ushort)new_cb);
}

template<typename T>
inline T pstsdk::db_writer<T>::bt_key(const std::vector<byte>& page, uint index)
{
    T key;
    memcpy(&key, &page[index * bt_entry_size(page)], sizeof(T));
    return key;
}

template<typename T>
inline void pstsdk::db_writer<T>::bt_set_key(std::vector<byte>& page, uint index, T key)
{
    memcpy(&page[index * bt_entry_size(page)], &key, sizeof(T));
}

template<typename T>
inline pstsdk::ulonglong pstsdk::db_writer<T>::bt_child(const std::vector<byte>& page, uint index)
{
    T address;
    memcpy(&address, &page[index * bt_entry_size(page) + 2 * sizeof(T)], sizeof(T));
    return address;
}

// Mirrors btree_node::binary_search: the last entry whose key is <= the target, or
// -1 when the target sorts below everything on the page.
template<typename T>
inline int pstsdk::db_writer<T>::bt_search(const std::vector<byte>& page, T key)
{
    uint start = 0;
    uint end = bt_count(page);
    uint mid = (start + end) / 2;

    while(mid < end)
    {
        T current = bt_key(page, mid);

        if(current < key)
            start = mid + 1;
        else if(current == key)
            return (int)mid;
        else
            end = mid;

        mid = (start + end) / 2;
    }

    return (int)mid - 1;
}

template<typename T>
inline std::vector<pstsdk::byte> pstsdk::db_writer<T>::read_page_raw(ulonglong address)
{
    std::vector<byte> page(disk::page_size);
    m_db->get_file().read(page, address);
    return page;
}

template<typename T>
inline void pstsdk::db_writer<T>::write_page_raw(ulonglong address, std::vector<byte>& page)
{
    disk::page<T>* p = reinterpret_cast<disk::page<T>*>(&page[0]);
    p->trailer.signature = disk::compute_signature((T)p->trailer.bid, (T)address);
    p->trailer.crc = disk::compute_crc(&page[0], disk::page<T>::page_data_size);

    m_db->get_file().write(page, address);

    // the store caches its BTree roots forever and never invalidates them, so any
    // page edit has to drop the cache or subsequent lookups read the old tree
    m_db->reset_page_cache();
    m_dirty = true;
}

template<typename T>
inline void pstsdk::db_writer<T>::bt_descend(ulonglong root, T key,
                                             std::vector<ulonglong>& path,
                                             std::vector<uint>& indices)
{
    ulonglong address = root;

    for(;;)
    {
        std::vector<byte> page = read_page_raw(address);
        int position = bt_search(page, key);

        if(position < 0)
            throw key_not_found<T>(key);

        path.push_back(address);
        indices.push_back((uint)position);

        if(bt_level(page) == 0)
        {
            if(bt_key(page, (uint)position) != key)
                throw key_not_found<T>(key);
            return;
        }

        address = bt_child(page, (uint)position);
    }
}

template<typename T>
inline void pstsdk::db_writer<T>::bt_propagate_key(const std::vector<ulonglong>& path,
                                                   const std::vector<uint>& indices,
                                                   size_t depth, T key)
{
    while(depth > 0)
    {
        size_t parent = depth - 1;
        std::vector<byte> page = read_page_raw(path[parent]);
        bt_set_key(page, indices[parent], key);
        write_page_raw(path[parent], page);

        // only a change to the parent's own first entry keeps rippling upward
        if(indices[parent] != 0)
            return;

        depth = parent;
    }
}

template<typename T>
inline void pstsdk::db_writer<T>::bt_remove(ulonglong root, T key)
{
    std::vector<ulonglong> path;
    std::vector<uint> indices;
    bt_descend(root, key, path, indices);

    // Bail before touching anything if this would cascade all the way up: the
    // removal below zeroes pages as it climbs, so discovering it at the root would
    // leave the tree half dismantled.
    bool empties_root = true;
    for(size_t depth = 0; depth < path.size() && empties_root; ++depth)
    {
        std::vector<byte> page = read_page_raw(path[depth]);
        empties_root = bt_count(page) == 1;
    }
    if(empties_root)
        throw database_corrupt("btree root emptied");

    for(size_t depth = path.size(); depth-- > 0;)
    {
        std::vector<byte> page = read_page_raw(path[depth]);
        const uint count = bt_count(page);
        const uint entry_size = bt_entry_size(page);
        const uint index = indices[depth];

        if(index + 1 < count)
            memmove(&page[index * entry_size], &page[(index + 1) * entry_size],
                    (count - index - 1) * entry_size);
        memset(&page[(count - 1) * entry_size], 0, entry_size);
        page[bt_meta] = (byte)(count - 1);

        if(count > 1)
        {
            write_page_raw(path[depth], page);

            // [MS-PST] 2.2.2.7.7.2 defines a nonleaf key as its child's first key,
            // so dropping entry zero leaves every ancestor naming a key that moved
            if(index == 0)
                bt_propagate_key(path, indices, depth, bt_key(page, 0));

            return;
        }

        // An empty page is not merely untidy: btree_node_nonleaf::first indexes
        // child zero unguarded and last underflows num_values() - 1, so end() is
        // poisoned as soon as one exists. Drop it from its parent instead.
        if(depth == 0)
            throw database_corrupt("btree root emptied");

        zero_extent(path[depth], disk::page_size);
    }
}

template<typename T>
inline void pstsdk::db_writer<T>::bt_find(ulonglong root, T key, ulonglong& address, uint& index)
{
    std::vector<ulonglong> path;
    std::vector<uint> indices;
    bt_descend(root, key, path, indices);

    address = path.back();
    index = indices.back();
}

template<typename T>
inline void pstsdk::db_writer<T>::nbt_remove(node_id nid)
{
    bt_remove(nbt_root(), (T)nid);
}

template<typename T>
inline void pstsdk::db_writer<T>::bbt_remove(block_id bid)
{
    bt_remove(bbt_root(), (T)(bid & ~(block_id(disk::block_id_attached_bit))));
}

template<typename T>
inline void pstsdk::db_writer<T>::bbt_set_size(block_id bid, ushort cb)
{
    ulonglong address;
    uint index;
    bt_find(bbt_root(), (T)(bid & ~(block_id(disk::block_id_attached_bit))), address, index);

    std::vector<byte> page = read_page_raw(address);
    disk::bbt_leaf_entry<T>* entry =
        reinterpret_cast<disk::bbt_leaf_entry<T>*>(&page[index * bt_entry_size(page)]);
    entry->size = cb;
    write_page_raw(address, page);
}

template<typename T>
inline void pstsdk::db_writer<T>::bbt_set_ref_count(block_id bid, ushort count)
{
    ulonglong address;
    uint index;
    bt_find(bbt_root(), (T)(bid & ~(block_id(disk::block_id_attached_bit))), address, index);

    std::vector<byte> page = read_page_raw(address);
    disk::bbt_leaf_entry<T>* entry =
        reinterpret_cast<disk::bbt_leaf_entry<T>*>(&page[index * bt_entry_size(page)]);
    entry->ref_count = count;
    write_page_raw(address, page);
}

template<typename T>
inline void pstsdk::db_writer<T>::collect_data_tree(block_id bid, std::vector<block_id>& blocks)
{
    if(bid == 0)
        return;

    blocks.push_back(bid);

    if(disk::bid_is_external(bid))
        return;

    std::vector<byte> raw = read_block(bid);
    const disk::extended_block<T>* xblock =
        reinterpret_cast<const disk::extended_block<T>*>(&raw[0]);

    if(xblock->block_type != disk::block_type_extended)
        throw unexpected_block("expected an extended block in a data tree");

    for(ushort i = 0; i < xblock->count; ++i)
        collect_data_tree(xblock->bid[i], blocks);
}

template<typename T>
inline void pstsdk::db_writer<T>::collect_subnode_tree(block_id bid, std::vector<block_id>& blocks)
{
    if(bid == 0)
        return;

    blocks.push_back(bid);

    std::vector<byte> raw = read_block(bid);
    const disk::sub_block<T, disk::sub_leaf_entry<T> >* sblock =
        reinterpret_cast<const disk::sub_block<T, disk::sub_leaf_entry<T> >*>(&raw[0]);

    if(sblock->block_type != disk::block_type_sub)
        throw unexpected_block("expected a subnode block in a subnode tree");

    if(sblock->level == 0)
    {
        for(ushort i = 0; i < sblock->count; ++i)
        {
            // a subnode owns a data tree and a subnode tree of its own, which is
            // how attachments and embedded messages hang off a message
            collect_data_tree(sblock->entry[i].data, blocks);
            collect_subnode_tree(sblock->entry[i].sub, blocks);
        }

        return;
    }

    const disk::sub_block<T, disk::sub_nonleaf_entry<T> >* nonleaf =
        reinterpret_cast<const disk::sub_block<T, disk::sub_nonleaf_entry<T> >*>(&raw[0]);

    for(ushort i = 0; i < nonleaf->count; ++i)
        collect_subnode_tree(nonleaf->entry[i].sub_block_bid, blocks);
}

template<typename T>
inline std::vector<pstsdk::block_id> pstsdk::db_writer<T>::external_blocks(block_id bid)
{
    std::vector<block_id> tree;
    collect_data_tree(bid, tree);

    std::vector<block_id> external;
    for(size_t i = 0; i < tree.size(); ++i)
        if(disk::bid_is_external(tree[i]))
            external.push_back(tree[i]);

    return external;
}


template<typename T>
inline typename pstsdk::db_writer<T>::data_ref pstsdk::db_writer<T>::node_ref(node_id nid)
{
    const node_info ni = m_db->lookup_node_info(nid);
    data_ref ref;
    ref.data = ni.data_bid;
    ref.sub = ni.sub_bid;
    return ref;
}

template<typename T>
inline typename pstsdk::db_writer<T>::data_ref
pstsdk::db_writer<T>::subnode_ref(block_id tree, node_id sub)
{
    std::vector<block_id> pending;
    pending.push_back(tree);

    while(!pending.empty())
    {
        const block_id bid = pending.back();
        pending.pop_back();

        if(bid == 0)
            continue;

        std::vector<byte> raw = read_block(bid);
        const disk::sub_block<T, disk::sub_leaf_entry<T> >* leaf =
            reinterpret_cast<const disk::sub_block<T, disk::sub_leaf_entry<T> >*>(&raw[0]);

        if(leaf->level != 0)
        {
            const disk::sub_block<T, disk::sub_nonleaf_entry<T> >* nonleaf =
                reinterpret_cast<const disk::sub_block<T, disk::sub_nonleaf_entry<T> >*>(&raw[0]);

            for(ushort i = 0; i < nonleaf->count; ++i)
                pending.push_back(nonleaf->entry[i].sub_block_bid);

            continue;
        }

        for(ushort i = 0; i < leaf->count; ++i)
        {
            if(leaf->entry[i].nid != sub)
                continue;

            data_ref ref;
            ref.data = leaf->entry[i].data;
            ref.sub = leaf->entry[i].sub;
            return ref;
        }
    }

    throw key_not_found<node_id>(sub);
}

template<typename T>
inline bool pstsdk::db_writer<T>::subnode_remove(block_id tree, node_id sub)
{
    if(tree == 0)
        throw key_not_found<node_id>(sub);

    std::vector<byte> raw = read_block(tree);
    disk::sub_block<T, disk::sub_leaf_entry<T> >* leaf =
        reinterpret_cast<disk::sub_block<T, disk::sub_leaf_entry<T> >*>(&raw[0]);

    if(leaf->level != 0)
    {
        disk::sub_block<T, disk::sub_nonleaf_entry<T> >* nonleaf =
            reinterpret_cast<disk::sub_block<T, disk::sub_nonleaf_entry<T> >*>(&raw[0]);

        for(ushort i = 0; i < nonleaf->count; ++i)
        {
            block_id child = nonleaf->entry[i].sub_block_bid;

            bool emptied = false;
            try { emptied = subnode_remove(child, sub); }
            catch(key_not_found<node_id>&) { continue; }

            if(!emptied)
                return false;

            if(i + 1 < nonleaf->count)
                memmove(&nonleaf->entry[i], &nonleaf->entry[i + 1],
                        (nonleaf->count - i - 1) * sizeof(disk::sub_nonleaf_entry<T>));
            memset(&nonleaf->entry[nonleaf->count - 1], 0, sizeof(disk::sub_nonleaf_entry<T>));
            --nonleaf->count;

            if(nonleaf->count == 0)
                return true;

            write_block(tree, raw);
            release_block(child);
            return false;
        }

        throw key_not_found<node_id>(sub);
    }

    for(ushort i = 0; i < leaf->count; ++i)
    {
        if(leaf->entry[i].nid != sub)
            continue;

        const block_id data = leaf->entry[i].data;
        const block_id owned = leaf->entry[i].sub;

        if(i + 1 < leaf->count)
            memmove(&leaf->entry[i], &leaf->entry[i + 1],
                    (leaf->count - i - 1) * sizeof(disk::sub_leaf_entry<T>));
        memset(&leaf->entry[leaf->count - 1], 0, sizeof(disk::sub_leaf_entry<T>));
        --leaf->count;

        // the entry is gone from the block before anything it pointed at is
        // released, so the store never references a scrubbed block
        if(leaf->count > 0)
            write_block(tree, raw);

        std::map<block_id, ushort> remaining;
        std::map<block_id, block_info> info;
        std::vector<block_id> order;
        plan_release(data, remaining, info, order);
        plan_release(owned, remaining, info, order);
        apply_release(remaining, info, order);

        return leaf->count == 0;
    }

    throw key_not_found<node_id>(sub);
}

template<typename T>
inline bool pstsdk::db_writer<T>::shrink_data_tail(block_id bid, size_t new_size)
{
    if(disk::bid_is_external(bid))
    {
        if(new_size == 0)
            return true;

        std::vector<byte> payload = read_block(bid);
        payload.resize(new_size);
        write_block(bid, payload);
        return false;
    }

    std::vector<byte> raw = read_block(bid);
    disk::extended_block<T>* xblock = reinterpret_cast<disk::extended_block<T>*>(&raw[0]);

    if(xblock->block_type != disk::block_type_extended)
        throw unexpected_block("expected an extended block in a data tree");

    if(xblock->count == 0)
        return true;

    const ushort last = (ushort)(xblock->count - 1);
    const block_id child = xblock->bid[last];
    const size_t was = m_db->lookup_block_info(child).size;

    if(!shrink_data_tail(child, new_size))
    {
        xblock->total_size -= (ulong)(was - new_size);
        write_block(bid, raw);
        return false;
    }

    // the child gave up its last row, so it leaves the tree with it
    xblock->total_size -= (ulong)was;
    xblock->bid[last] = 0;
    --xblock->count;

    if(xblock->count == 0)
        return true;

    write_block(bid, raw);
    release_block(child);
    return false;
}

template<typename T>
inline void pstsdk::db_writer<T>::block_children(block_id bid, std::vector<block_id>& children)
{
    if(disk::bid_is_external(bid))
        return;

    std::vector<byte> raw = read_block(bid);

    if(raw[0] == disk::block_type_extended)
    {
        const disk::extended_block<T>* xblock =
            reinterpret_cast<const disk::extended_block<T>*>(&raw[0]);

        for(ushort i = 0; i < xblock->count; ++i)
            children.push_back(xblock->bid[i]);

        return;
    }

    if(raw[0] != disk::block_type_sub)
        throw unexpected_block("unknown internal block type");

    const disk::sub_block<T, disk::sub_leaf_entry<T> >* leaf =
        reinterpret_cast<const disk::sub_block<T, disk::sub_leaf_entry<T> >*>(&raw[0]);

    if(leaf->level == 0)
    {
        for(ushort i = 0; i < leaf->count; ++i)
        {
            children.push_back(leaf->entry[i].data);
            children.push_back(leaf->entry[i].sub);
        }

        return;
    }

    const disk::sub_block<T, disk::sub_nonleaf_entry<T> >* nonleaf =
        reinterpret_cast<const disk::sub_block<T, disk::sub_nonleaf_entry<T> >*>(&raw[0]);

    for(ushort i = 0; i < nonleaf->count; ++i)
        children.push_back(nonleaf->entry[i].sub_block_bid);
}

// A block that keeps a reference keeps its children with it, so the walk stops at
// anything that survives. Descending past one and freeing what it points at would
// pull the data out from under whoever still owns it. Real stores do share
// extended and subnode blocks, so this is not hypothetical.
template<typename T>
inline void pstsdk::db_writer<T>::plan_release(block_id root,
                                               std::map<block_id, ushort>& remaining,
                                               std::map<block_id, block_info>& info,
                                               std::vector<block_id>& order)
{
    std::vector<block_id> pending;
    pending.push_back(root);

    while(!pending.empty())
    {
        const block_id raw = pending.back();
        pending.pop_back();

        if(raw == 0)
            continue;

        const block_id bid = raw & ~(block_id(disk::block_id_attached_bit));

        if(remaining.find(bid) == remaining.end())
        {
            const block_info bi = m_db->lookup_block_info(bid);
            info[bid] = bi;
            remaining[bid] = bi.ref_count;
            order.push_back(bid);
        }

        if(remaining[bid] <= disk::block_unreferenced)
            throw database_corrupt("block released more often than it is referenced");

        --remaining[bid];

        if(remaining[bid] > disk::block_unreferenced)
            continue;

        block_children(bid, pending);
    }
}

template<typename T>
inline void pstsdk::db_writer<T>::apply_release(const std::map<block_id, ushort>& remaining,
                                                const std::map<block_id, block_info>& info,
                                                const std::vector<block_id>& order)
{
    for(size_t i = 0; i < order.size(); ++i)
    {
        const block_id bid = order[i];
        const ushort count = remaining.find(bid)->second;

        if(count > disk::block_unreferenced)
        {
            bbt_set_ref_count(bid, count);
            continue;
        }

        const block_info& bi = info.find(bid)->second;
        const ulonglong address = bi.address;
        const size_t extent = disk::align_disk<T>(bi.size);

        bbt_remove(bid);
        zero_extent(address, extent);
    }
}

template<typename T>
inline void pstsdk::db_writer<T>::release_block(block_id bid)
{
    std::map<block_id, ushort> remaining;
    std::map<block_id, block_info> info;
    std::vector<block_id> order;

    plan_release(bid, remaining, info, order);
    apply_release(remaining, info, order);
}

template<typename T>
inline void pstsdk::db_writer<T>::delete_node(node_id nid)
{
    const node_info ni = m_db->lookup_node_info(nid);

    std::map<block_id, ushort> remaining;
    std::map<block_id, block_info> info;
    std::vector<block_id> order;

    plan_release(ni.data_bid, remaining, info, order);
    plan_release(ni.sub_bid, remaining, info, order);

    nbt_remove(nid);
    apply_release(remaining, info, order);
}

template<typename T>
inline void pstsdk::db_writer<T>::collect_pages(ulonglong address, std::vector<ulonglong>& pages)
{
    pages.push_back(address);

    std::vector<byte> page = read_page_raw(address);
    if(bt_level(page) == 0)
        return;

    for(uint i = 0; i < bt_count(page); ++i)
        collect_pages(bt_child(page, i), pages);
}

// Rather than working out where every kind of allocation map page is supposed to
// sit, ask the bytes. A page carries its type twice and a CRC over its contents,
// so anything that validates is a real page of some kind and is left alone. That
// covers the AMaps, the DList and the deprecated PMap and FMap pages without
// this code having to know their spacing.
template<typename T>
inline bool pstsdk::db_writer<T>::looks_like_page(ulonglong address)
{
    std::vector<byte> page(disk::page_size);

    try { m_db->get_file().read(page, address); }
    catch(std::out_of_range&) { return false; }

    const disk::page<T>* p = reinterpret_cast<const disk::page<T>*>(&page[0]);

    if(p->trailer.page_type != p->trailer.page_type_repeat)
        return false;

    switch(p->trailer.page_type)
    {
    case disk::page_type_bbt:
    case disk::page_type_nbt:
    case disk::page_type_fmap:
    case disk::page_type_pmap:
    case disk::page_type_amap:
    case disk::page_type_fpmap:
    case disk::page_type_dlist:
        break;
    default:
        return false;
    }

    return p->trailer.crc == disk::compute_crc(&page[0], disk::page<T>::page_data_size);
}

template<typename T>
inline pstsdk::ulonglong pstsdk::db_writer<T>::wipe_free_space()
{
    const ulonglong eof = m_db->get_header().root_info.ibFileEof;
    const ulonglong first = disk::first_amap_page_location;

    std::vector<std::pair<ulonglong, ulonglong> > live;

    // the header, the DList and everything reserved ahead of the first AMap
    live.push_back(std::make_pair((ulonglong)0, first));

    std::vector<ulonglong> pages;
    collect_pages(nbt_root(), pages);
    collect_pages(bbt_root(), pages);
    for(size_t i = 0; i < pages.size(); ++i)
        live.push_back(std::make_pair(pages[i], pages[i] + disk::page_size));

    std::shared_ptr<bbt_page> root = m_db->read_bbt_root();
    for(const_blockinfo_iterator i = root->begin(); i != root->end(); ++i)
        live.push_back(std::make_pair((*i).address,
                                      (*i).address + disk::align_disk<T>((*i).size)));

    std::sort(live.begin(), live.end());

    std::vector<std::pair<ulonglong, ulonglong> > keep;
    ulonglong at = 0;

    for(size_t i = 0; i <= live.size(); ++i)
    {
        const ulonglong stop = i < live.size() ? live[i].first : eof;

        ulonglong page = at < first ? first : at;
        if((page - first) % disk::page_size)
            page += disk::page_size - ((page - first) % disk::page_size);

        for(; page + disk::page_size <= stop; page += disk::page_size)
            if(looks_like_page(page))
                keep.push_back(std::make_pair(page, page + disk::page_size));

        if(i < live.size() && live[i].second > at)
            at = live[i].second;
    }

    live.insert(live.end(), keep.begin(), keep.end());
    std::sort(live.begin(), live.end());

    ulonglong wiped = 0;
    at = 0;

    for(size_t i = 0; i < live.size(); ++i)
    {
        if(live[i].first > at)
        {
            zero_extent(at, (size_t)(live[i].first - at));
            wiped += live[i].first - at;
        }

        if(live[i].second > at)
            at = live[i].second;
    }

    if(eof > at)
    {
        zero_extent(at, (size_t)(eof - at));
        wiped += eof - at;
    }

    return wiped;
}

template<typename T>
inline void pstsdk::db_writer<T>::zero_extent(ulonglong address, size_t size)
{
    if(size == 0)
        return;

    std::vector<byte> zeroes(size, 0);
    m_db->get_file().write(zeroes, address);
    m_dirty = true;
}

template<typename T>
inline void pstsdk::db_writer<T>::commit()
{
    if(!m_dirty)
        return;

    disk::header<T>& h = m_db->get_header();
    h.root_info.fAMapValid = disk::invalid_amap;
    ++h.dwUnique;
    stamp_header_crc();

    std::vector<byte> buffer(sizeof(disk::header<T>));
    memcpy(&buffer[0], &h, sizeof(disk::header<T>));
    m_db->get_file().write(buffer, 0);

    m_db->reset_page_cache();
    m_dirty = false;
}

template<>
inline void pstsdk::db_writer<pstsdk::ulong>::stamp_header_crc()
{
    disk::header<ulong>& h = m_db->get_header();
    h.dwCRCPartial = disk::compute_crc(
        reinterpret_cast<byte*>(&h) + disk::header_crc_locations<ulong>::start,
        disk::header_crc_locations<ulong>::length);
}

template<>
inline void pstsdk::db_writer<pstsdk::ulonglong>::stamp_header_crc()
{
    disk::header<ulonglong>& h = m_db->get_header();
    h.dwCRCPartial = disk::compute_crc(
        reinterpret_cast<byte*>(&h) + disk::header_crc_locations<ulonglong>::partial_start,
        disk::header_crc_locations<ulonglong>::partial_length);
    h.dwCRCFull = disk::compute_crc(
        reinterpret_cast<byte*>(&h) + disk::header_crc_locations<ulonglong>::full_start,
        disk::header_crc_locations<ulonglong>::full_length);
}
//! \endcond

#endif
