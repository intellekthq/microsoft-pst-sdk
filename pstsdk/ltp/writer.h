//! \file
//! \brief In place edits of heaps, BTHs, property contexts and table contexts
//!
//! Built on \ref db_writer, so the same rules apply: nothing moves, nothing
//! grows, and a shared block is refused rather than copied.
//!
//! Heap allocations shrink the way [MS-PST] 2.3.1.5 describes, by sliding the
//! allocations above them down and leaving the freed entry zero length. The
//! allocation array never shrinks, so every heap id in the block keeps its
//! meaning. The block itself must not shrink either: \ref heap_impl::size reads
//! the page map as everything from page_map_offset to the end of the block, so
//! trimming the block would truncate the map.
//! \ingroup ltp

#ifndef PSTSDK_LTP_WRITER_H
#define PSTSDK_LTP_WRITER_H

#include <cstring>
#include <vector>

#include "pstsdk/util/errors.h"
#include "pstsdk/util/primitives.h"

#include "pstsdk/ndb/writer.h"

namespace pstsdk
{

//! \cond write_api

//! \brief Edits the heap carried by a single node
//!
//! Resolves heap ids to a block and an offset, and writes whole heap blocks back
//! through \ref db_writer.
//! \tparam T ulonglong for a Unicode store, ulong for an ANSI store
//! \ingroup ltp_heaprelated
template<typename T>
class heap_writer
{
public:
    heap_writer(db_writer<T>& writer, node_id nid)
        : m_writer(writer), m_blocks(writer.node_external_blocks(nid)) { }

    //! \brief The heap's root allocation, as recorded in its first block
    heap_id root_id();

    std::vector<byte> read_block(uint page) { return m_writer.read_block(m_blocks.at(page)); }
    void write_block(uint page, const std::vector<byte>& data)
        { m_writer.write_block(m_blocks.at(page), data); }

    //! \brief Locate an allocation within its heap block
    //! \param[in] id The allocation to resolve
    //! \param[out] page The heap block holding it
    //! \param[out] offset Its offset within that block
    //! \param[out] size Its length in bytes
    void locate(heap_id id, uint& page, size_t& offset, size_t& size);

    //! \brief Read an allocation's bytes
    std::vector<byte> read_alloc(heap_id id);

    //! \brief Overwrite an allocation without changing its length
    void write_alloc(heap_id id, const std::vector<byte>& data);

    //! \brief Shrink an allocation, sliding everything above it down
    //!
    //! Passing zero frees it outright, which is what the format calls a
    //! zero length allocation rather than a removed one.
    //! \throws can_not_resize if asked to grow
    //! \param[in] id The allocation to shrink
    //! \param[in] size Its new length, at most its current one
    void shrink_alloc(heap_id id, size_t size);

private:
    db_writer<T>& m_writer;
    std::vector<block_id> m_blocks;
};

//! \brief Overwrite a fixed width property held inline in a property context
//!
//! A \ref disk::prop_entry carries values of four bytes or fewer in its id field
//! rather than pointing at them, so this is a same size edit. Used for the folder
//! counts and the message flags that have to track a delete.
//! \throws key_not_found<prop_id> if the property is not present
//! \ingroup ltp_pcrelated
template<typename T>
void pc_set_inline(db_writer<T>& writer, node_id nid, prop_id id, ulong value);

//! \brief Remove a row from a table context
//!
//! Copies the last row over the doomed one and drops the matrix by a row, rather
//! than compacting, so only two row index entries move instead of every entry
//! above the deletion point. Row order carries no meaning in a table context.
//! \throws key_not_found<row_id> if the row is not in the table
//! \param[in] writer The store to edit
//! \param[in] nid The table's node
//! \param[in] id The row to remove, which for a folder table is the item's node id
//! \ingroup ltp_tcrelated
template<typename T>
void tc_remove_row(db_writer<T>& writer, node_id nid, row_id id);

//! \endcond

} // end pstsdk namespace

//! \cond write_api
template<typename T>
inline pstsdk::heap_id pstsdk::heap_writer<T>::root_id()
{
    std::vector<byte> first = read_block(0);
    const disk::heap_first_header* header =
        reinterpret_cast<const disk::heap_first_header*>(&first[0]);

    if(header->signature != disk::heap_signature)
        throw database_corrupt("invalid heap signature");

    return header->root_id;
}

template<typename T>
inline void pstsdk::heap_writer<T>::locate(heap_id id, uint& page, size_t& offset, size_t& size)
{
    page = get_heap_page(id);
    const uint index = get_heap_index(id);

    std::vector<byte> block = read_block(page);
    const disk::heap_page_header* header =
        reinterpret_cast<const disk::heap_page_header*>(&block[0]);
    const disk::heap_page_map* map =
        reinterpret_cast<const disk::heap_page_map*>(&block[header->page_map_offset]);

    if(index >= map->num_allocs)
        throw std::length_error("heap index past num_allocs");

    offset = map->allocs[index];
    size = map->allocs[index + 1] - map->allocs[index];
}

template<typename T>
inline std::vector<pstsdk::byte> pstsdk::heap_writer<T>::read_alloc(heap_id id)
{
    uint page;
    size_t offset;
    size_t size;
    locate(id, page, offset, size);

    std::vector<byte> block = read_block(page);
    return std::vector<byte>(block.begin() + offset, block.begin() + offset + size);
}

template<typename T>
inline void pstsdk::heap_writer<T>::write_alloc(heap_id id, const std::vector<byte>& data)
{
    uint page;
    size_t offset;
    size_t size;
    locate(id, page, offset, size);

    if(data.size() != size)
        throw can_not_resize("write_alloc cannot change an allocation's size");

    std::vector<byte> block = read_block(page);
    if(size > 0)
        memcpy(&block[offset], &data[0], size);
    write_block(page, block);
}

template<typename T>
inline void pstsdk::heap_writer<T>::shrink_alloc(heap_id id, size_t size)
{
    uint page;
    size_t offset;
    size_t current;
    locate(id, page, offset, current);

    if(size > current)
        throw can_not_resize("shrink_alloc cannot grow an allocation");
    if(size == current)
        return;

    const size_t freed = current - size;
    const uint index = get_heap_index(id);

    std::vector<byte> block = read_block(page);
    disk::heap_page_header* header = reinterpret_cast<disk::heap_page_header*>(&block[0]);
    disk::heap_page_map* map =
        reinterpret_cast<disk::heap_page_map*>(&block[header->page_map_offset]);

    const size_t used = map->allocs[map->num_allocs];

    // slide everything above the allocation down over the bytes being given up
    memmove(&block[offset + size], &block[offset + current], used - offset - current);
    memset(&block[used - freed], 0, freed);

    for(uint i = index + 1; i <= map->num_allocs; ++i)
        map->allocs[i] = (ushort)(map->allocs[i] - freed);

    if(size == 0)
        ++map->num_frees;

    write_block(page, block);
}

namespace pstsdk
{
namespace detail
{

//! \brief How a particular BTH lays its entries out on disk
//!
//! The strides come from the BTH header rather than from sizeof, because the
//! disk structs do not all pack the way the format does. sizeof
//! bth_leaf_entry<row_id, ushort> is 8, while an ANSI row index really steps
//! 6 bytes at a time.
struct bth_layout
{
    size_t key_size;
    size_t value_size;
    byte levels;
    heap_id root;

    size_t leaf_stride() const { return key_size + value_size; }
    size_t nonleaf_stride() const { return key_size + sizeof(heap_id); }
    size_t value_offset(uint index) const { return index * leaf_stride() + key_size; }
};

template<typename T>
inline bth_layout bth_read_layout(heap_writer<T>& heap, heap_id bth_root)
{
    std::vector<byte> raw = heap.read_alloc(bth_root);
    const disk::bth_header* header = reinterpret_cast<const disk::bth_header*>(&raw[0]);

    if(header->bth_signature != disk::heap_sig_bth)
        throw database_corrupt("invalid BTH signature");

    bth_layout layout;
    layout.key_size = header->key_size;
    layout.value_size = header->entry_size;
    layout.levels = header->num_levels;
    layout.root = header->root;
    return layout;
}

//! Every BTH key in the format is four bytes or fewer
inline ulong bth_read_at(const std::vector<byte>& alloc, size_t offset, size_t width)
{
    ulong value = 0;
    memcpy(&value, &alloc[offset], width);
    return value;
}

inline void bth_write_at(std::vector<byte>& alloc, size_t offset, size_t width, ulong value)
{
    memcpy(&alloc[offset], &value, width);
}

//! Walk a BTH to the allocation holding an exact key, recording the path
template<typename T>
inline void bth_descend(heap_writer<T>& heap, const bth_layout& layout, ulong key,
                        std::vector<heap_id>& path, std::vector<uint>& indices)
{
    heap_id current = layout.root;

    for(byte level = layout.levels; level > 0; --level)
    {
        std::vector<byte> alloc = heap.read_alloc(current);
        const uint count = (uint)(alloc.size() / layout.nonleaf_stride());

        if(count == 0 || key < bth_read_at(alloc, 0, layout.key_size))
            throw key_not_found<ulong>(key);

        uint position = 0;
        while(position + 1 < count &&
              bth_read_at(alloc, (position + 1) * layout.nonleaf_stride(), layout.key_size) <= key)
            ++position;

        path.push_back(current);
        indices.push_back(position);
        current = (heap_id)bth_read_at(alloc, position * layout.nonleaf_stride() + layout.key_size,
                                       sizeof(heap_id));
    }

    std::vector<byte> alloc = heap.read_alloc(current);
    const uint count = (uint)(alloc.size() / layout.leaf_stride());

    for(uint i = 0; i < count; ++i)
    {
        if(bth_read_at(alloc, i * layout.leaf_stride(), layout.key_size) != key)
            continue;

        path.push_back(current);
        indices.push_back(i);
        return;
    }

    throw key_not_found<ulong>(key);
}

//! Drop one leaf entry, fixing the ancestors that named the leaf by its first key
template<typename T>
inline void bth_remove_key(heap_writer<T>& heap, const bth_layout& layout, ulong key)
{
    std::vector<heap_id> path;
    std::vector<uint> indices;
    bth_descend(heap, layout, key, path, indices);

    const size_t stride = layout.leaf_stride();
    std::vector<byte> alloc = heap.read_alloc(path.back());
    const uint count = (uint)(alloc.size() / stride);
    const uint index = indices.back();

    if(index + 1 < count)
        memmove(&alloc[index * stride], &alloc[(index + 1) * stride],
                (count - index - 1) * stride);

    heap.write_alloc(path.back(), alloc);
    heap.shrink_alloc(path.back(), (count - 1) * stride);

    if(count > 1)
    {
        if(index != 0 || path.size() < 2)
            return;

        std::vector<byte> survivors = heap.read_alloc(path.back());
        const ulong first = bth_read_at(survivors, 0, layout.key_size);

        for(size_t depth = path.size() - 1; depth-- > 0;)
        {
            std::vector<byte> parent = heap.read_alloc(path[depth]);
            bth_write_at(parent, indices[depth] * layout.nonleaf_stride(), layout.key_size, first);
            heap.write_alloc(path[depth], parent);

            if(indices[depth] != 0)
                return;
        }

        return;
    }

    // the leaf is empty, so take it out of its parent, and keep going if that
    // empties the parent too
    const size_t nonleaf = layout.nonleaf_stride();
    for(size_t depth = path.size() - 1; depth-- > 0;)
    {
        std::vector<byte> parent = heap.read_alloc(path[depth]);
        const uint parent_count = (uint)(parent.size() / nonleaf);
        const uint parent_index = indices[depth];

        if(parent_index + 1 < parent_count)
            memmove(&parent[parent_index * nonleaf], &parent[(parent_index + 1) * nonleaf],
                    (parent_count - parent_index - 1) * nonleaf);

        heap.write_alloc(path[depth], parent);
        heap.shrink_alloc(path[depth], (parent_count - 1) * nonleaf);

        if(parent_count > 1)
            return;
    }
}

} // end detail namespace
} // end pstsdk namespace

template<typename T>
inline void pstsdk::pc_set_inline(db_writer<T>& writer, node_id nid, prop_id id, ulong value)
{
    heap_writer<T> heap(writer, nid);
    detail::bth_layout layout = detail::bth_read_layout(heap, heap.root_id());

    std::vector<heap_id> path;
    std::vector<uint> indices;

    // the BTH works in erased four byte keys, so restore the caller's key type
    try { detail::bth_descend(heap, layout, id, path, indices); }
    catch(key_not_found<ulong>&) { throw key_not_found<prop_id>(id); }

    std::vector<byte> alloc = heap.read_alloc(path.back());
    const size_t value_at = layout.value_offset(indices.back());

    // a prop_entry is a two byte type followed by the value, and anything four
    // bytes or under lives in that field rather than behind it
    const ulong type = detail::bth_read_at(alloc, value_at, sizeof(ushort));
    if(type != prop_type_long && type != prop_type_boolean)
        throw not_implemented("pc_set_inline only handles inline fixed width properties");

    detail::bth_write_at(alloc, value_at + sizeof(ushort), sizeof(ulong), value);
    heap.write_alloc(path.back(), alloc);
}

template<typename T>
inline void pstsdk::tc_remove_row(db_writer<T>& writer, node_id nid, row_id id)
{
    heap_writer<T> heap(writer, nid);
    const heap_id root = heap.root_id();

    std::vector<byte> raw = heap.read_alloc(root);
    const disk::tc_header* header = reinterpret_cast<const disk::tc_header*>(&raw[0]);

    if(header->signature != disk::heap_sig_tc)
        throw database_corrupt("not a table context");

    const size_t cb_per_row = header->size_offsets[disk::tc_offsets_bitmap];
    const heap_id row_btree = header->row_btree_id;
    const heapnode_id matrix = header->row_matrix_id;

    if(matrix == 0)
        throw key_not_found<row_id>(id);

    // Every table in every store under test/ keeps its matrix inline. A folder
    // big enough to outgrow a heap allocation moves it into a subnode, which
    // needs a block level shrink instead and is not handled yet.
    if(is_subnode_id(matrix))
        throw not_implemented("row matrix held in a subnode");

    detail::bth_layout layout = detail::bth_read_layout(heap, row_btree);

    std::vector<byte> rows = heap.read_alloc(matrix);
    const size_t count = cb_per_row ? rows.size() / cb_per_row : 0;

    if(count == 0)
        throw key_not_found<row_id>(id);

    std::vector<heap_id> path;
    std::vector<uint> indices;
    detail::bth_descend(heap, layout, id, path, indices);

    std::vector<byte> leaf = heap.read_alloc(path.back());
    const size_t target = detail::bth_read_at(leaf, layout.value_offset(indices.back()),
                                              layout.value_size);
    const size_t last = count - 1;

    if(target != last)
    {
        // the moved row keeps its id, so the index has to learn its new position
        row_id moved;
        memcpy(&moved, &rows[last * cb_per_row], sizeof(row_id));
        memcpy(&rows[target * cb_per_row], &rows[last * cb_per_row], cb_per_row);
        heap.write_alloc(matrix, rows);

        std::vector<heap_id> moved_path;
        std::vector<uint> moved_indices;
        detail::bth_descend(heap, layout, moved, moved_path, moved_indices);

        std::vector<byte> moved_leaf = heap.read_alloc(moved_path.back());
        detail::bth_write_at(moved_leaf, layout.value_offset(moved_indices.back()),
                             layout.value_size, (ulong)target);
        heap.write_alloc(moved_path.back(), moved_leaf);
    }

    heap.shrink_alloc(matrix, last * cb_per_row);
    detail::bth_remove_key(heap, layout, id);

    if(last > 0)
        return;

    // an emptied table carries neither a matrix nor a row index root on disk,
    // which is what one that was never populated looks like
    std::vector<byte> bth_raw = heap.read_alloc(row_btree);
    reinterpret_cast<disk::bth_header*>(&bth_raw[0])->root = 0;
    heap.write_alloc(row_btree, bth_raw);

    std::vector<byte> header_raw = heap.read_alloc(root);
    reinterpret_cast<disk::tc_header*>(&header_raw[0])->row_matrix_id = 0;
    heap.write_alloc(root, header_raw);
}
//! \endcond

#endif
