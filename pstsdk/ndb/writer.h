//! \file
//! \brief In place edits of an open store
//!
//! Every edit here writes back to the address it read from, at the same size or
//! smaller. That is what lets the library delete items without owning an
//! allocator: nothing is ever relocated, no block or page is ever created, and
//! the space that falls out is recovered by the client's AMap rebuild rather
//! than by us.
//!
//! The two rules that fall out of having no allocator:
//! - a block with more than one reference can be unlinked but never modified,
//!   because there is nowhere to copy it to first
//! - a block can shrink but never grow
//! \ingroup ndb

#ifndef PSTSDK_NDB_WRITER_H
#define PSTSDK_NDB_WRITER_H

#include <algorithm>
#include <cstring>
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

    //! \brief Overwrite a range of the file with zeroes
    //! \param[in] address The offset to start at
    //! \param[in] size The number of bytes to zero
    void zero_extent(ulonglong address, size_t size);

    //! \brief Remove a node's entry from the NBT
    //! \throws key_not_found<node_id> if the node is not in the tree
    //! \param[in] nid The node to unlink
    void nbt_remove(node_id nid);

    //! \brief Remove a block's entry from the BBT
    //! \throws key_not_found<block_id> if the block is not in the tree
    //! \param[in] bid The block to unlink
    void bbt_remove(block_id bid);

    //! \brief Overwrite the unaligned size recorded for a block
    //! \param[in] bid The block to update
    //! \param[in] cb The block's new unaligned size
    void bbt_set_size(block_id bid, ushort cb);

    //! \brief Overwrite the reference count recorded for a block
    //! \param[in] bid The block to update
    //! \param[in] count The new count, where \ref disk::block_unreferenced means free
    void bbt_set_ref_count(block_id bid, ushort count);

    //! \brief Drop one reference to a block, scrubbing it when the last one goes
    //!
    //! A block still owned by someone else keeps its bytes and only loses a
    //! reference. The last owner's departure zeroes the block's whole aligned
    //! extent, which is what actually removes the data from the store.
    //! \param[in] bid The block to release
    void release_block(block_id bid);

    //! \brief Unlink a node and scrub everything only it owned
    //!
    //! Collects the node's data tree and subnode tree first, because neither is
    //! reachable once the NBT entry is gone, then unlinks before scrubbing so the
    //! store is structurally valid at every point in between.
    //! \throws key_not_found<node_id> if the node is not in the tree
    //! \param[in] nid The node to delete
    void delete_node(node_id nid);

    //! \brief The external blocks making up a node's data, in logical order
    //!
    //! A heap spans these blocks, one heap block apiece, so this is how the LTP
    //! layer turns a heap page number into something it can write.
    //! \param[in] nid The node to walk
    std::vector<block_id> node_external_blocks(node_id nid);

    //! \brief Mark the header as needing a restamp on the next \ref commit
    void touch() { m_dirty = true; }

    //! \brief Flush the header and drop the store's cached BTree roots
    //!
    //! Invalidates the AMaps so the client rebuilds them, which is what actually
    //! returns the freed slots. Does nothing if no edit has been made.
    void commit();

private:
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

    //! Walk to the leaf holding an exact key, recording the path taken
    void bt_descend(ulonglong root, T key, std::vector<ulonglong>& path, std::vector<uint>& indices);
    void bt_remove(ulonglong root, T key);
    //! Retarget the ancestors that named a page by its old first key
    void bt_propagate_key(const std::vector<ulonglong>& path, const std::vector<uint>& indices,
                          size_t depth, T key);
    //! Locate a leaf entry so a caller can patch its non-key fields in place
    void bt_find(ulonglong root, T key, ulonglong& address, uint& index);

    //! Walk a data tree, appending every block that makes it up
    void collect_data_tree(block_id bid, std::vector<block_id>& blocks);
    //! Walk a subnode tree, appending its blocks and those of every subnode
    void collect_subnode_tree(block_id bid, std::vector<block_id>& blocks);

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
inline std::vector<pstsdk::block_id> pstsdk::db_writer<T>::node_external_blocks(node_id nid)
{
    node_info ni = m_db->lookup_node_info(nid);

    std::vector<block_id> tree;
    collect_data_tree(ni.data_bid, tree);

    std::vector<block_id> external;
    for(size_t i = 0; i < tree.size(); ++i)
        if(disk::bid_is_external(tree[i]))
            external.push_back(tree[i]);

    return external;
}

template<typename T>
inline void pstsdk::db_writer<T>::release_block(block_id bid)
{
    block_info bi = m_db->lookup_block_info(bid);

    if(bi.ref_count > disk::block_unreferenced + 1)
    {
        bbt_set_ref_count(bid, (ushort)(bi.ref_count - 1));
        return;
    }

    const ulonglong address = bi.address;
    const size_t extent = disk::align_disk<T>(bi.size);

    bbt_remove(bid);
    zero_extent(address, extent);
}

template<typename T>
inline void pstsdk::db_writer<T>::delete_node(node_id nid)
{
    node_info ni = m_db->lookup_node_info(nid);

    std::vector<block_id> blocks;
    collect_data_tree(ni.data_bid, blocks);
    collect_subnode_tree(ni.sub_bid, blocks);

    // a block reachable twice within one node must still only lose one reference
    std::sort(blocks.begin(), blocks.end());
    blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());

    nbt_remove(nid);

    for(size_t i = 0; i < blocks.size(); ++i)
        release_block(blocks[i]);
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
