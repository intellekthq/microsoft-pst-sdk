#include <algorithm>
#include <cstdlib>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "pstsdk/ltp.h"
#include "pstsdk/ndb.h"
#include "pstsdk/pst.h"

#include "test.h"

namespace
{

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string narrow(const std::wstring& s) { return std::string(s.begin(), s.end()); }

// Everything here runs against a copy. The delete design assumes the caller made
// one, and it keeps the sample stores pristine when a test trips an assert.
std::wstring copy_sample(const std::string& sample, const std::string& tag)
{
    std::string copy = tag + "-" + sample;
    std::filesystem::copy_file(sample, copy, std::filesystem::copy_options::overwrite_existing);
    return widen(copy);
}

std::vector<pstsdk::byte> slurp(const std::wstring& path)
{
    std::ifstream in(narrow(path), std::ios::binary);
    return std::vector<pstsdk::byte>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

std::vector<pstsdk::block_id> all_bids(const pstsdk::shared_db_ptr& db)
{
    std::vector<pstsdk::block_id> bids;
    std::shared_ptr<pstsdk::bbt_page> root = db->read_bbt_root();

    for(pstsdk::const_blockinfo_iterator i = root->begin(); i != root->end(); ++i)
        bids.push_back((*i).id);

    return bids;
}

// Rewriting every block with the bytes it already holds has to be a no-op outside
// the header. Failing this means trailer placement, encode direction, the CRC
// domain or the ANSI/Unicode trailer field order is wrong, and it says so before
// there is any delete logic to blame it on.
template<typename T>
void test_block_roundtrip(const std::string& sample)
{
    using namespace pstsdk;

    std::wstring path = copy_sample(sample, "roundtrip");
    std::vector<byte> before = slurp(path);
    size_t shared = 0;

    {
        std::shared_ptr<file> f(new file(path, true));
        shared_db_ptr db = open_database(f);
        std::shared_ptr<database_impl<T> > impl = std::dynamic_pointer_cast<database_impl<T> >(db);
        assert(impl);

        db_writer<T> writer(impl);
        std::vector<block_id> bids = all_bids(db);
        assert(!bids.empty());

        for(size_t i = 0; i < bids.size(); ++i)
        {
            std::vector<byte> payload = writer.read_block(bids[i]);

            if(db->lookup_block_info(bids[i]).ref_count > disk::block_unreferenced + 1)
            {
                bool refused = false;
                try { writer.write_block(bids[i], payload); }
                catch(shared_block&) { refused = true; }
                assert(refused);
                ++shared;
                continue;
            }

            writer.write_block(bids[i], payload);
        }

        writer.commit();
    }

    // every sample store shares the blocks behind its empty tables, so the refusal
    // above is exercised rather than merely present
    assert(shared > 0);

    std::vector<byte> after = slurp(path);
    assert(after.size() == before.size());

    for(size_t i = sizeof(disk::header<T>); i < after.size(); ++i)
        assert(after[i] == before[i]);

    const disk::header<T>* was = reinterpret_cast<const disk::header<T>*>(&before[0]);
    const disk::header<T>* is = reinterpret_cast<const disk::header<T>*>(&after[0]);
    assert(is->dwUnique == was->dwUnique + 1);
    assert(is->root_info.fAMapValid == disk::invalid_amap);
    assert(is->root_info.ibFileEof == was->root_info.ibFileEof);

    // and the store still opens under PSTSDK_VALIDATION_LEVEL_FULL, which checks
    // every page CRC, block CRC and signature on the way through
    {
        shared_db_ptr db = open_database(path);
        assert(std::dynamic_pointer_cast<database_impl<T> >(db));
    }

    std::filesystem::remove(narrow(path));
}

// Shrinking is the only resize write_block allows, and the interesting case is a
// shrink that crosses a 64 byte slot boundary, because that moves the trailer and
// leaves the old one behind unless the whole extent is cleared first.
template<typename T>
void test_block_shrink(const std::string& sample)
{
    using namespace pstsdk;

    std::wstring path = copy_sample(sample, "shrink");
    size_t shrunk = 0;

    {
        std::shared_ptr<file> f(new file(path, true));
        shared_db_ptr db = open_database(f);
        std::shared_ptr<database_impl<T> > impl = std::dynamic_pointer_cast<database_impl<T> >(db);
        db_writer<T> writer(impl);

        std::vector<block_id> bids = all_bids(db);
        for(size_t i = 0; i < bids.size(); ++i)
        {
            block_info bi = db->lookup_block_info(bids[i]);
            if(bi.ref_count > disk::block_unreferenced + 1 || bi.size <= 64)
                continue;

            // enough to guarantee at least one slot is given back
            const size_t target = bi.size - 64;
            const ulonglong old_trailer =
                bi.address + disk::align_disk<T>(bi.size) - sizeof(disk::block_trailer<T>);

            std::vector<byte> payload = writer.read_block(bids[i]);
            payload.resize(target);
            writer.write_block(bids[i], payload);

            assert(db->lookup_block_info(bids[i]).size == target);
            assert(writer.read_block(bids[i]) == payload);

            // the trailer moved, so nothing may be left where it used to sit
            const ulonglong new_end = bi.address + disk::align_disk<T>(target);
            if(old_trailer >= new_end)
            {
                std::vector<byte> stale(sizeof(disk::block_trailer<T>));
                file check(path);
                check.read(stale, old_trailer);
                for(size_t b = 0; b < stale.size(); ++b)
                    assert(stale[b] == 0);
            }

            ++shrunk;
        }

        block_info bi = db->lookup_block_info(bids[0]);
        bool refused = false;
        try { writer.write_block(bids[0], std::vector<byte>(bi.size + 1, 0)); }
        catch(can_not_resize&) { refused = true; }
        assert(refused);

        writer.commit();
    }

    assert(shrunk > 0);
    std::filesystem::remove(narrow(path));
}

std::vector<pstsdk::node_id> all_nids(const pstsdk::shared_db_ptr& db)
{
    std::vector<pstsdk::node_id> nids;
    std::shared_ptr<pstsdk::nbt_page> root = db->read_nbt_root();

    for(pstsdk::const_nodeinfo_iterator i = root->begin(); i != root->end(); ++i)
        nids.push_back((*i).id);

    return nids;
}

// Reads the BTree straight off disk rather than through the SDK, so it can assert
// the structural invariants the SDK's own readers quietly depend on. Returns the
// page's first key and appends every leaf key it passes, left to right.
template<typename T>
T check_bt_page(pstsdk::file& f, pstsdk::ulonglong address, std::vector<T>& keys)
{
    using namespace pstsdk;

    const size_t meta = disk::page<T>::page_data_size - sizeof(T);
    std::vector<byte> p(disk::page_size);
    f.read(p, address);

    const uint count = p[meta];
    const uint entry_size = p[meta + 2];
    const uint level = p[meta + 3];

    assert(count > 0);

    T previous = 0;
    for(uint i = 0; i < count; ++i)
    {
        T key;
        memcpy(&key, &p[i * entry_size], sizeof(T));
        assert(i == 0 || previous < key);
        previous = key;

        if(level == 0)
        {
            keys.push_back(key);
            continue;
        }

        T child;
        memcpy(&child, &p[i * entry_size + 2 * sizeof(T)], sizeof(T));
        assert(check_bt_page<T>(f, child, keys) == key);
    }

    T first;
    memcpy(&first, &p[0], sizeof(T));
    return first;
}

// Drops one node id at a time, each from a fresh copy, and checks that what is
// left is still a well formed tree. Deleting a node without also unlinking what
// points at it leaves the store semantically inconsistent, which is fine: this
// exercises nbt_remove on its own.
template<typename T>
void test_nbt_remove(const std::string& sample)
{
    using namespace pstsdk;

    std::vector<node_id> original;
    {
        shared_db_ptr db = open_database(widen(sample));
        original = all_nids(db);
    }
    assert(original.size() > 1);

    for(size_t victim = 0; victim < original.size(); ++victim)
    {
        std::wstring path = copy_sample(sample, "nbtremove");

        {
            std::shared_ptr<file> f(new file(path, true));
            std::shared_ptr<database_impl<T> > impl =
                std::dynamic_pointer_cast<database_impl<T> >(open_database(f));
            db_writer<T> writer(impl);
            writer.nbt_remove(original[victim]);
            writer.commit();
        }

        std::vector<byte> raw = slurp(path);
        const disk::header<T>* h = reinterpret_cast<const disk::header<T>*>(&raw[0]);

        std::vector<T> keys;
        {
            file f(path);
            check_bt_page<T>(f, h->root_info.brefNBT.ib, keys);
        }
        assert(keys.size() == original.size() - 1);

        // and the SDK's own iterators agree, which is what catches an empty page:
        // end() is built from root->last(), so one poisons the whole range
        shared_db_ptr db = open_database(path);
        std::vector<node_id> remaining = all_nids(db);
        assert(remaining.size() == original.size() - 1);

        for(size_t i = 0; i < remaining.size(); ++i)
            assert(remaining[i] != original[victim]);

        bool missing = false;
        try { db->lookup_node_info(original[victim]); }
        catch(key_not_found<node_id>&) { missing = true; }
        assert(missing);

        db.reset();
        std::filesystem::remove(narrow(path));
    }
}

// Removing one entry never empties a page in stores this small, so the recursive
// pruning path only shows up if the tree is drained. Takes every node but the last
// from a single copy, revalidating the whole tree after each one.
template<typename T>
void test_nbt_drain(const std::string& sample)
{
    using namespace pstsdk;

    std::wstring path = copy_sample(sample, "nbtdrain");
    std::vector<node_id> nids;
    ulonglong root;

    {
        std::vector<byte> raw = slurp(path);
        root = reinterpret_cast<const disk::header<T>*>(&raw[0])->root_info.brefNBT.ib;
        shared_db_ptr db = open_database(path);
        nids = all_nids(db);
    }
    assert(nids.size() > 2);

    std::shared_ptr<file> f(new file(path, true));
    std::shared_ptr<database_impl<T> > impl =
        std::dynamic_pointer_cast<database_impl<T> >(open_database(f));
    db_writer<T> writer(impl);

    for(size_t i = 0; i + 1 < nids.size(); ++i)
    {
        writer.nbt_remove(nids[i]);
        writer.commit();

        file check(path);
        std::vector<T> keys;
        check_bt_page<T>(check, root, keys);
        assert(keys.size() == nids.size() - i - 1);
    }

    // one entry left, and taking it would leave no tree to point at
    bool refused = false;
    try { writer.nbt_remove(nids.back()); }
    catch(database_corrupt&) { refused = true; }
    assert(refused);

    // the refusal has to be clean: the tree is still exactly as it was
    file check(path);
    std::vector<T> keys;
    check_bt_page<T>(check, root, keys);
    assert(keys.size() == 1);

    std::filesystem::remove(narrow(path));
}

std::map<pstsdk::block_id, pstsdk::block_info> bbt_snapshot(const pstsdk::shared_db_ptr& db)
{
    std::map<pstsdk::block_id, pstsdk::block_info> blocks;
    std::shared_ptr<pstsdk::bbt_page> root = db->read_bbt_root();

    for(pstsdk::const_blockinfo_iterator i = root->begin(); i != root->end(); ++i)
        blocks[(*i).id] = *i;

    return blocks;
}

template<typename T>
std::vector<pstsdk::byte> read_raw_block(pstsdk::file& f,
                                         const std::map<pstsdk::block_id, pstsdk::block_info>& bbt,
                                         pstsdk::block_id bid)
{
    using namespace pstsdk;

    std::map<block_id, block_info>::const_iterator entry =
        bbt.find(bid & ~block_id(disk::block_id_attached_bit));
    assert(entry != bbt.end());

    std::vector<byte> buffer(disk::align_disk<T>(entry->second.size));
    f.read(buffer, entry->second.address);
    buffer.resize(entry->second.size);
    return buffer;
}

template<typename T>
void count_subnode_tree(pstsdk::file& f, const std::map<pstsdk::block_id, pstsdk::block_info>& bbt,
                        pstsdk::block_id bid, std::map<pstsdk::block_id, unsigned>& refs,
                        std::set<pstsdk::block_id>& seen);

template<typename T>
void count_data_tree(pstsdk::file& f, const std::map<pstsdk::block_id, pstsdk::block_info>& bbt,
                     pstsdk::block_id bid, std::map<pstsdk::block_id, unsigned>& refs,
                     std::set<pstsdk::block_id>& seen)
{
    using namespace pstsdk;

    if(bid == 0)
        return;

    const block_id key = bid & ~block_id(disk::block_id_attached_bit);
    ++refs[key];

    if(disk::bid_is_external(bid))
        return;

    // count every edge, but walk a shared block's contents only once
    if(!seen.insert(key).second)
        return;

    std::vector<byte> raw = read_raw_block<T>(f, bbt, bid);
    const disk::extended_block<T>* xblock =
        reinterpret_cast<const disk::extended_block<T>*>(&raw[0]);

    for(pstsdk::ushort i = 0; i < xblock->count; ++i)
        count_data_tree<T>(f, bbt, xblock->bid[i], refs, seen);
}

template<typename T>
void count_subnode_tree(pstsdk::file& f, const std::map<pstsdk::block_id, pstsdk::block_info>& bbt,
                        pstsdk::block_id bid, std::map<pstsdk::block_id, unsigned>& refs,
                        std::set<pstsdk::block_id>& seen)
{
    using namespace pstsdk;

    if(bid == 0)
        return;

    const block_id key = bid & ~block_id(disk::block_id_attached_bit);
    ++refs[key];

    if(!seen.insert(key).second)
        return;

    std::vector<byte> raw = read_raw_block<T>(f, bbt, bid);
    const disk::sub_block<T, disk::sub_leaf_entry<T> >* leaf =
        reinterpret_cast<const disk::sub_block<T, disk::sub_leaf_entry<T> >*>(&raw[0]);

    if(leaf->level == 0)
    {
        for(pstsdk::ushort i = 0; i < leaf->count; ++i)
        {
            count_data_tree<T>(f, bbt, leaf->entry[i].data, refs, seen);
            count_subnode_tree<T>(f, bbt, leaf->entry[i].sub, refs, seen);
        }

        return;
    }

    const disk::sub_block<T, disk::sub_nonleaf_entry<T> >* nonleaf =
        reinterpret_cast<const disk::sub_block<T, disk::sub_nonleaf_entry<T> >*>(&raw[0]);

    for(pstsdk::ushort i = 0; i < nonleaf->count; ++i)
        count_subnode_tree<T>(f, bbt, nonleaf->entry[i].sub_block_bid, refs, seen);
}

// Rebuilds the true reference graph from the NBT down and holds the BBT to it.
// This is the oracle that catches a delete which collected too few blocks: an
// uncollected one is still in the tree but nothing points at it any more.
template<typename T>
void check_refcounts(const pstsdk::shared_db_ptr& db, const std::wstring& path)
{
    using namespace pstsdk;

    std::map<block_id, block_info> bbt = bbt_snapshot(db);
    std::map<block_id, unsigned> refs;
    std::set<block_id> seen;
    file f(path);

    std::shared_ptr<nbt_page> root = db->read_nbt_root();
    for(const_nodeinfo_iterator i = root->begin(); i != root->end(); ++i)
    {
        count_data_tree<T>(f, bbt, (*i).data_bid, refs, seen);
        count_subnode_tree<T>(f, bbt, (*i).sub_bid, refs, seen);
    }

    for(std::map<block_id, block_info>::const_iterator i = bbt.begin(); i != bbt.end(); ++i)
    {
        const unsigned actual = refs.count(i->first) ? refs[i->first] : 0;
        assert(actual > 0);
        assert(i->second.ref_count == actual + disk::block_unreferenced);
    }
}

// Deletes every node in turn, each from a fresh copy, and diffs the BBT across
// the delete. A block that lost its last owner has to be gone from the tree and
// zeroed on disk; a block someone else still owns has to survive intact with its
// count down by exactly one.
template<typename T>
void test_delete_node(const std::string& sample)
{
    using namespace pstsdk;

    std::vector<node_id> nids;
    {
        shared_db_ptr db = open_database(widen(sample));
        nids = all_nids(db);
    }

    size_t scrubbed = 0;
    size_t decremented = 0;

    for(size_t victim = 0; victim < nids.size(); ++victim)
    {
        std::wstring path = copy_sample(sample, "delnode");

        std::map<block_id, block_info> before;
        {
            shared_db_ptr db = open_database(path);
            before = bbt_snapshot(db);
        }

        {
            std::shared_ptr<file> f(new file(path, true));
            std::shared_ptr<database_impl<T> > impl =
                std::dynamic_pointer_cast<database_impl<T> >(open_database(f));
            db_writer<T> writer(impl);
            writer.delete_node(nids[victim]);
            writer.commit();
        }

        shared_db_ptr db = open_database(path);
        std::map<block_id, block_info> after = bbt_snapshot(db);
        file raw(path);

        for(std::map<block_id, block_info>::const_iterator i = before.begin(); i != before.end(); ++i)
        {
            std::map<block_id, block_info>::const_iterator survivor = after.find(i->first);

            if(survivor == after.end())
            {
                std::vector<byte> extent(disk::align_disk<T>(i->second.size));
                raw.read(extent, i->second.address);

                for(size_t b = 0; b < extent.size(); ++b)
                    assert(extent[b] == 0);

                ++scrubbed;
                continue;
            }

            assert(survivor->second.address == i->second.address);
            assert(survivor->second.ref_count == i->second.ref_count ||
                   survivor->second.ref_count == i->second.ref_count - 1);

            if(survivor->second.ref_count < i->second.ref_count)
                ++decremented;
        }

        assert(all_nids(db).size() == nids.size() - 1);
        check_refcounts<T>(db, path);

        db.reset();
        std::filesystem::remove(narrow(path));
    }

    assert(scrubbed > 0);
    // the empty-table blocks every sample store shares, losing one owner apiece
    assert(decremented > 0);
}

// Folder counts are PT_I4, which a property context carries inside the entry
// rather than behind it, so updating one after a delete is a same size poke.
template<typename T>
void test_pc_set_inline(const std::string& sample)
{
    using namespace pstsdk;

    std::wstring path = copy_sample(sample, "pcinline");
    node_id folder = 0;

    {
        shared_db_ptr db = open_database(path);
        std::vector<node_id> nids = all_nids(db);

        for(size_t i = 0; i < nids.size() && folder == 0; ++i)
            if(get_nid_type(nids[i]) == nid_type_folder)
                folder = nids[i];
    }
    assert(folder != 0);

    {
        std::shared_ptr<file> f(new file(path, true));
        std::shared_ptr<database_impl<T> > impl =
            std::dynamic_pointer_cast<database_impl<T> >(open_database(f));
        db_writer<T> writer(impl);

        pc_set_inline(writer, folder, (prop_id)PR_CONTENT_COUNT, 4242);

        bool missing = false;
        try { pc_set_inline(writer, folder, (prop_id)0x7ffe, 1); }
        catch(key_not_found<prop_id>&) { missing = true; }
        assert(missing);

        writer.commit();
    }

    shared_db_ptr db = open_database(path);
    property_bag bag(db->lookup_node(folder));
    assert(bag.read_prop<slong>(PR_CONTENT_COUNT) == 4242);

    // the poke must not have disturbed anything else in the heap
    assert(bag.read_prop<slong>(PR_CONTENT_UNREAD) >= 0);
    check_refcounts<T>(db, path);

    db.reset();
    std::filesystem::remove(narrow(path));
}

std::vector<pstsdk::row_id> table_rows(const pstsdk::node& owner, pstsdk::node_id sub)
{
    pstsdk::table tc(owner.lookup(sub));
    std::vector<pstsdk::row_id> rows;

    for(size_t i = 0; i < tc.size(); ++i)
        rows.push_back(tc[i].get_row_id());

    return rows;
}

std::vector<pstsdk::row_id> table_rows(const pstsdk::shared_db_ptr& db, pstsdk::node_id nid)
{
    pstsdk::table tc(db->lookup_node(nid));
    std::vector<pstsdk::row_id> rows;

    for(size_t i = 0; i < tc.size(); ++i)
        rows.push_back(tc[i].get_row_id());

    return rows;
}

std::vector<pstsdk::row_id> table_rows_or_empty(const pstsdk::shared_db_ptr& db, pstsdk::node_id nid)
{
    try { return table_rows(db, nid); }
    catch(pstsdk::key_not_found<pstsdk::node_id>&) { return std::vector<pstsdk::row_id>(); }
}

// Reads a table's row index straight out of the heap and holds it against the row
// matrix. The SDK never calls lookup_row, so enumerating rows proves nothing about
// the index, which is the half Outlook actually uses to find a message.
void check_row_index(const pstsdk::shared_db_ptr& db, pstsdk::node_id nid)
{
    using namespace pstsdk;

    node n = db->lookup_node(nid);
    std::vector<byte> page(n.get_page_size(0));
    n.read(page, 0, 0);

    const disk::heap_first_header* heap =
        reinterpret_cast<const disk::heap_first_header*>(&page[0]);
    const disk::heap_page_map* map =
        reinterpret_cast<const disk::heap_page_map*>(&page[heap->page_map_offset]);

    // heap ids number allocations from one, and only page zero is in play here
    const disk::tc_header* tc = reinterpret_cast<const disk::tc_header*>(
        &page[map->allocs[get_heap_index(heap->root_id)]]);

    const size_t cb_per_row = tc->size_offsets[disk::tc_offsets_bitmap];
    const disk::bth_header* bth = reinterpret_cast<const disk::bth_header*>(
        &page[map->allocs[get_heap_index(tc->row_btree_id)]]);

    size_t rows = 0;
    const byte* matrix = 0;
    if(tc->row_matrix_id != 0)
    {
        assert(!is_subnode_id(tc->row_matrix_id));
        const uint index = get_heap_index(tc->row_matrix_id);
        matrix = &page[map->allocs[index]];
        rows = (map->allocs[index + 1] - map->allocs[index]) / cb_per_row;
    }

    if(bth->root == 0)
    {
        assert(rows == 0);
        return;
    }

    // the sample stores are all single level; anything deeper needs a walk
    assert(bth->num_levels == 0);

    const uint leaf = get_heap_index(bth->root);
    const size_t stride = bth->key_size + bth->entry_size;
    const byte* entries = &page[map->allocs[leaf]];
    const size_t count = (map->allocs[leaf + 1] - map->allocs[leaf]) / stride;

    assert(count == rows);

    std::vector<size_t> seen;
    pstsdk::ulong previous = 0;

    for(size_t i = 0; i < count; ++i)
    {
        pstsdk::ulong key = 0;
        size_t position = 0;
        memcpy(&key, entries + i * stride, bth->key_size);
        memcpy(&position, entries + i * stride + bth->key_size, bth->entry_size);

        assert(i == 0 || previous < key);
        previous = key;

        assert(position < rows);
        pstsdk::ulong at_position = 0;
        memcpy(&at_position, matrix + position * cb_per_row, sizeof(pstsdk::ulong));
        assert(at_position == key);

        seen.push_back(position);
    }

    // every row is indexed exactly once
    std::sort(seen.begin(), seen.end());
    for(size_t i = 0; i < seen.size(); ++i)
        assert(seen[i] == i);
}

// Takes every row out of every populated table, one per copy, and checks that the
// survivors are exactly the rows that were there before minus the one removed.
// The row moved by swap-with-last is the interesting survivor: its index changed,
// so the row index has to have been retargeted.
template<typename T>
void test_tc_remove_row(const std::string& sample)
{
    using namespace pstsdk;

    std::vector<std::pair<node_id, std::vector<row_id> > > tables;
    {
        shared_db_ptr db = open_database(widen(sample));
        std::vector<node_id> nids = all_nids(db);

        for(size_t i = 0; i < nids.size(); ++i)
        {
            const nid_type type = get_nid_type(nids[i]);
            if(type != nid_type_contents_table && type != nid_type_hierarchy_table &&
               type != nid_type_associated_contents_table)
                continue;

            std::vector<row_id> rows = table_rows(db, nids[i]);
            if(!rows.empty())
                tables.push_back(std::make_pair(nids[i], rows));
        }
    }
    assert(!tables.empty());

    size_t emptied = 0;

    for(size_t t = 0; t < tables.size(); ++t)
    {
        const std::vector<row_id>& original = tables[t].second;

        for(size_t victim = 0; victim < original.size(); ++victim)
        {
            std::wstring path = copy_sample(sample, "tcrow");

            {
                std::shared_ptr<file> f(new file(path, true));
                std::shared_ptr<database_impl<T> > impl =
                    std::dynamic_pointer_cast<database_impl<T> >(open_database(f));
                db_writer<T> writer(impl);
                tc_remove_row(writer, tables[t].first, original[victim]);
                writer.commit();
            }

            shared_db_ptr db = open_database(path);
            std::vector<row_id> remaining = table_rows(db, tables[t].first);
            assert(remaining.size() == original.size() - 1);

            std::vector<row_id> expected;
            for(size_t i = 0; i < original.size(); ++i)
                if(i != victim)
                    expected.push_back(original[i]);

            std::sort(expected.begin(), expected.end());
            std::sort(remaining.begin(), remaining.end());
            assert(expected == remaining);

            if(remaining.empty())
                ++emptied;

            check_refcounts<T>(db, path);
            check_row_index(db, tables[t].first);

            db.reset();
            std::filesystem::remove(narrow(path));
        }
    }

    // the one-row tables in these stores make the drop-to-empty path real
    assert(emptied > 0);
}

// Opens everything the pst layer can reach and touches enough of each item to
// force the blocks behind it to be read and validated.
void walk_store(const std::wstring& path)
{
    using namespace pstsdk;

    pst store(path);

    for(pst::folder_iterator i = store.folder_begin(); i != store.folder_end(); ++i)
    {
        // Bind the folder rather than working through the iterator: it yields by
        // value, and const_table_row_iter::equal compares the table pointer, so
        // begin() and end() taken off two temporaries never meet.
        folder f = *i;
        f.get_name();

        for(folder::message_iterator m = f.message_begin(); m != f.message_end(); ++m)
        {
            message item = *m;
            item.get_attachment_count();
            item.get_recipient_count();

            if(item.has_subject())
                item.get_subject();
        }
    }
}

// The permute-encoded form of a string, which is how it actually sits on disk
std::vector<pstsdk::byte> encoded_needle(const std::wstring& text)
{
    using namespace pstsdk;

    std::vector<byte> raw;
    for(size_t i = 0; i < text.size(); ++i)
    {
        raw.push_back((byte)(text[i] & 0xff));
        raw.push_back((byte)((text[i] >> 8) & 0xff));
    }

    if(!raw.empty())
        disk::permute(&raw[0], (pstsdk::ulong)raw.size(), true);

    return raw;
}

// Occurrences anywhere in the file, free space included. This is the measure
// that matters once the wipe has run.
size_t count_occurrences(const std::vector<pstsdk::byte>& hay,
                         const std::vector<pstsdk::byte>& needle)
{
    if(needle.empty() || hay.size() < needle.size())
        return 0;

    size_t hits = 0;
    for(size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if(memcmp(&hay[i], &needle[0], needle.size()) == 0)
            ++hits;

    return hits;
}

// Occurrences that land inside a block the store still references. Free space is
// deliberately excluded: real stores already carry stale plaintext there from
// whatever the original client deleted, and an in place edit does not clear it.
size_t count_in_live_blocks(const std::vector<pstsdk::byte>& hay,
                            const std::vector<pstsdk::byte>& needle,
                            const std::map<pstsdk::block_id, pstsdk::block_info>& bbt)
{
    using namespace pstsdk;

    if(needle.empty() || hay.size() < needle.size())
        return 0;

    size_t hits = 0;
    for(size_t i = 0; i + needle.size() <= hay.size(); ++i)
    {
        if(memcmp(&hay[i], &needle[0], needle.size()) != 0)
            continue;

        for(std::map<block_id, block_info>::const_iterator b = bbt.begin(); b != bbt.end(); ++b)
            if(i >= b->second.address && i < b->second.address + b->second.size)
            {
                ++hits;
                break;
            }
    }

    return hits;
}

// Deletes every message in the store, one per copy, through the public entry
// point. This is the first test that holds the whole stack to account at once:
// the node is gone, the folder's row and count agree, the row index still lines
// up with the matrix, no block is orphaned, and the store still reads.
template<typename T>
void test_delete_message(const std::string& sample)
{
    using namespace pstsdk;

    std::vector<node_id> messages;
    {
        shared_db_ptr db = open_database(widen(sample));
        std::vector<node_id> nids = all_nids(db);

        for(size_t i = 0; i < nids.size(); ++i)
            if(get_nid_type(nids[i]) == nid_type_message)
                messages.push_back(nids[i]);
    }
    assert(!messages.empty());

    for(size_t victim = 0; victim < messages.size(); ++victim)
    {
        std::wstring path = copy_sample(sample, "delmsg");
        const node_id message = messages[victim];

        node_id folder = 0;
        std::wstring subject;
        bool unique_subject = false;
        {
            shared_db_ptr db = open_database(path);
            folder = db->lookup_node_info(message).parent_id;

            // Only assert on a subject no other message shares, otherwise a
            // surviving copy would look like a leak.
            size_t sharing = 0;
            for(size_t i = 0; i < messages.size(); ++i)
            {
                property_bag other(db->lookup_node(messages[i]));
                if(!other.prop_exists(PR_SUBJECT_W))
                    continue;

                const std::wstring text = other.read_prop<std::wstring>(PR_SUBJECT_W);
                if(messages[i] == message)
                    subject = text;
                else if(text == subject && !text.empty())
                    ++sharing;
            }

            unique_subject = sharing == 0 && subject.size() > 4;
        }

        {
            std::shared_ptr<file> f(new file(path, true));
            shared_db_ptr db = open_database(f);
            delete_message(db, message);
        }

        shared_db_ptr db = open_database(path);

        bool missing = false;
        try { db->lookup_node_info(message); }
        catch(key_not_found<node_id>&) { missing = true; }
        assert(missing);

        if(folder != 0)
        {
            const node_id contents = make_nid(nid_type_contents_table, get_nid_index(folder));
            std::vector<row_id> rows = table_rows(db, contents);

            for(size_t i = 0; i < rows.size(); ++i)
                assert(rows[i] != message);

            property_bag bag(db->lookup_node(folder));
            assert(bag.read_prop<slong>(PR_CONTENT_COUNT) == (slong)rows.size());

            check_row_index(db, contents);
        }

        check_refcounts<T>(db, path);

        // A table cell wider than the row points at a heap allocation holding the
        // cached subject in clear text. Removing the row has to free it, not just
        // orphan it, or the text stays in a block the store still reads.
        if(unique_subject)
            assert(count_in_live_blocks(slurp(path), encoded_needle(subject),
                                        bbt_snapshot(db)) == 0);

        db.reset();

        walk_store(path);
        std::filesystem::remove(narrow(path));
    }
}

// Deletes each folder below the root, one per copy. A folder takes its property
// context, its three tables and everything underneath it, so the check is that
// none of those node ids survive and the store still reads.
template<typename T>
void test_delete_folder(const std::string& sample)
{
    using namespace pstsdk;

    std::vector<node_id> folders;
    {
        shared_db_ptr db = open_database(widen(sample));
        std::vector<node_id> nids = all_nids(db);

        for(size_t i = 0; i < nids.size(); ++i)
            if(get_nid_type(nids[i]) == nid_type_folder && nids[i] != nid_root_folder)
                folders.push_back(nids[i]);
    }
    assert(!folders.empty());

    for(size_t victim = 0; victim < folders.size(); ++victim)
    {
        std::wstring path = copy_sample(sample, "delfolder");
        const node_id target = folders[victim];

        node_id parent = 0;
        std::vector<node_id> doomed;
        {
            shared_db_ptr db = open_database(path);
            parent = db->lookup_node_info(target).parent_id;

            doomed.push_back(target);
            const nid_type tables[] = { nid_type_contents_table, nid_type_hierarchy_table,
                                        nid_type_associated_contents_table };

            for(size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); ++i)
                doomed.push_back(make_nid(tables[i], get_nid_index(target)));

            std::vector<row_id> messages =
                table_rows_or_empty(db, make_nid(nid_type_contents_table, get_nid_index(target)));
            for(size_t i = 0; i < messages.size(); ++i)
                doomed.push_back(messages[i]);
        }

        {
            std::shared_ptr<file> f(new file(path, true));
            shared_db_ptr db = open_database(f);
            delete_folder(db, target);
        }

        shared_db_ptr db = open_database(path);

        for(size_t i = 0; i < doomed.size(); ++i)
        {
            bool missing = false;
            try { db->lookup_node_info(doomed[i]); }
            catch(key_not_found<node_id>&) { missing = true; }
            assert(missing);
        }

        if(parent != 0)
        {
            const node_id hierarchy = make_nid(nid_type_hierarchy_table, get_nid_index(parent));
            std::vector<row_id> rows = table_rows_or_empty(db, hierarchy);

            for(size_t i = 0; i < rows.size(); ++i)
                assert(rows[i] != target);
        }

        check_refcounts<T>(db, path);
        db.reset();

        walk_store(path);
        std::filesystem::remove(narrow(path));
    }
}

// Deletes every attachment of every message that has one, from a fresh copy each
// time. The message has to survive with one fewer attachment and the rest intact,
// and the attachment's own subnode has to be gone rather than orphaned.
template<typename T>
void test_delete_attachment(const std::string& sample)
{
    using namespace pstsdk;

    std::vector<std::pair<node_id, std::vector<row_id> > > owners;
    {
        shared_db_ptr db = open_database(widen(sample));
        std::vector<node_id> nids = all_nids(db);

        for(size_t i = 0; i < nids.size(); ++i)
        {
            if(get_nid_type(nids[i]) != nid_type_message)
                continue;

            std::vector<row_id> rows;
            try { rows = table_rows(db->lookup_node(nids[i]), nid_attachment_table); }
            catch(key_not_found<node_id>&) { continue; }

            if(!rows.empty())
                owners.push_back(std::make_pair(nids[i], rows));
        }
    }
    assert(!owners.empty());


    for(size_t m = 0; m < owners.size(); ++m)
    {
        const std::vector<row_id>& all = owners[m].second;

        for(size_t victim = 0; victim < all.size(); ++victim)
        {
            std::wstring path = copy_sample(sample, "delatt");

            {
                std::shared_ptr<file> f(new file(path, true));
                shared_db_ptr db = open_database(f);
                delete_attachment(db, owners[m].first, (node_id)all[victim]);
            }

            shared_db_ptr db = open_database(path);
            node owner = db->lookup_node(owners[m].first);
            std::vector<row_id> left = table_rows(owner, nid_attachment_table);
            assert(left.size() == all.size() - 1);

            for(size_t i = 0; i < left.size(); ++i)
            {
                assert(left[i] != all[victim]);
                // the survivors still resolve to real subnodes
                owner.lookup((node_id)left[i]);
            }

            bool gone = false;
            try { owner.lookup((node_id)all[victim]); }
            catch(key_not_found<node_id>&) { gone = true; }
            assert(gone);

            message item(db->lookup_node(owners[m].first));
            assert(item.get_attachment_count() == left.size());

            // These stores do not record PR_HASATTACH anywhere, in the message
            // or in the folder's cached row, so a client derives it from the
            // attachment table.
            if(left.empty())
            {
                property_bag bag(db->lookup_node(owners[m].first));
                if(bag.prop_exists(PR_HASATTACH))
                    assert(!bag.read_prop<slong>(PR_HASATTACH));
            }

            check_refcounts<T>(db, path);
            db.reset();

            walk_store(path);
            std::filesystem::remove(narrow(path));
        }
    }

}

// The one test that speaks to the point of the feature: after deleting an item
// and wiping, its text is absent from the whole file, not merely from the blocks
// the store still references. submessage.pst ships with copies of a subject in
// space its original client freed, so this covers inherited residue too.
template<typename T>
void test_wipe_free_space(const std::string& sample, const std::wstring& text)
{
    using namespace pstsdk;

    std::wstring path = copy_sample(sample, "wipe");
    const std::vector<byte> needle = encoded_needle(text);
    assert(count_occurrences(slurp(path), needle) > 0);

    node_id victim = 0;
    {
        shared_db_ptr db = open_database(path);
        std::vector<node_id> nids = all_nids(db);

        for(size_t i = 0; i < nids.size() && victim == 0; ++i)
        {
            if(get_nid_type(nids[i]) != nid_type_message)
                continue;

            // get_subject strips the two byte prefix the raw property carries
            message item(db->lookup_node(nids[i]));
            if(item.has_subject() && item.get_subject() == text)
                victim = nids[i];
        }
    }
    assert(victim != 0);

    ulonglong wiped = 0;
    {
        std::shared_ptr<file> f(new file(path, true));
        shared_db_ptr db = open_database(f);
        delete_message(db, victim);
    }
    {
        std::shared_ptr<file> f(new file(path, true));
        shared_db_ptr db = open_database(f);
        wiped = wipe_free_space(db);
    }
    assert(wiped > 0);

    // gone from the file outright, not just from what the store reads
    assert(count_occurrences(slurp(path), needle) == 0);

    shared_db_ptr db = open_database(path);
    check_refcounts<T>(db, path);
    db.reset();

    walk_store(path);
    std::filesystem::remove(narrow(path));
}

// Every store under test/ keeps its row matrices inline, so nothing here reaches
// the subnode path, which is the one a real folder takes past a few dozen
// messages. Point PSTSDK_TEST_CORPUS at a directory of real stores to cover it:
//
//   PSTSDK_TEST_CORPUS=~/corpus ninja -C build test
//
// Each store is copied, then every message in it is deleted one at a time, with
// the reference graph checked as it goes.
template<typename T>
void drain_store(const std::string& source)
{
    using namespace pstsdk;

    const std::wstring path = widen(source + ".draining");
    std::filesystem::copy_file(source, narrow(path),
                               std::filesystem::copy_options::overwrite_existing);

    size_t deleted = 0;

    for(;;)
    {
        std::vector<node_id> left;
        {
            shared_db_ptr db = open_database(path);
            std::vector<node_id> nids = all_nids(db);

            for(size_t i = 0; i < nids.size(); ++i)
                if(get_nid_type(nids[i]) == nid_type_message)
                    left.push_back(nids[i]);
        }

        if(left.empty())
            break;

        {
            std::shared_ptr<file> f(new file(path, true));
            shared_db_ptr db = open_database(f);
            delete_message(db, left[0]);
        }

        ++deleted;

        if(deleted % 25 == 0 || left.size() == 1)
        {
            shared_db_ptr db = open_database(path);
            check_refcounts<T>(db, path);
            db.reset();
            walk_store(path);
        }
    }

    ulonglong wiped = 0;
    {
        std::shared_ptr<file> f(new file(path, true));
        shared_db_ptr db = open_database(f);
        wiped = wipe_free_space(db);
    }
    {
        shared_db_ptr db = open_database(path);
        check_refcounts<T>(db, path);
        db.reset();
        walk_store(path);
    }

    assert(deleted > 0);
    std::wcout << L"  wiped " << wiped << L" bytes after draining" << std::endl;
    std::wcout << L"  drained " << deleted << L" messages from "
               << widen(source) << std::endl;

    std::filesystem::remove(narrow(path));
}

// Strips every attachment from every message, then confirms the messages are all
// still there and readable. Real stores carry the attachment shapes the samples
// do not: many per message, embedded messages, and blocks big enough to need an
// extended data tree.
template<typename T>
void strip_attachments(const std::string& source)
{
    using namespace pstsdk;

    const std::wstring path = widen(source + ".stripping");
    std::filesystem::copy_file(source, narrow(path),
                               std::filesystem::copy_options::overwrite_existing);

    size_t removed = 0;
    size_t messages = 0;

    for(;;)
    {
        node_id owner = 0;
        row_id victim = 0;
        {
            shared_db_ptr db = open_database(path);
            std::vector<node_id> nids = all_nids(db);

            for(size_t i = 0; i < nids.size() && owner == 0; ++i)
            {
                if(get_nid_type(nids[i]) != nid_type_message)
                    continue;

                std::vector<row_id> rows;
                try { rows = table_rows(db->lookup_node(nids[i]), nid_attachment_table); }
                catch(key_not_found<node_id>&) { continue; }

                if(rows.empty())
                    continue;

                owner = nids[i];
                victim = rows[0];
            }

            if(owner == 0)
            {
                messages = 0;
                std::shared_ptr<nbt_page> root = db->read_nbt_root();
                for(const_nodeinfo_iterator i = root->begin(); i != root->end(); ++i)
                    if(get_nid_type((*i).id) == nid_type_message)
                        ++messages;
            }
        }

        if(owner == 0)
            break;

        {
            std::shared_ptr<file> f(new file(path, true));
            shared_db_ptr db = open_database(f);
            delete_attachment(db, owner, (node_id)victim);
        }

        ++removed;

        if(removed % 25 == 0)
        {
            shared_db_ptr db = open_database(path);
            check_refcounts<T>(db, path);
            db.reset();
            walk_store(path);
        }
    }

    {
        shared_db_ptr db = open_database(path);
        check_refcounts<T>(db, path);
        db.reset();
        walk_store(path);
    }

    std::wcout << L"  stripped " << removed << L" attachments from "
               << widen(source) << L", " << messages << L" messages intact" << std::endl;

    std::filesystem::remove(narrow(path));
}

void test_corpus()
{
    const char* corpus = getenv("PSTSDK_TEST_CORPUS");
    if(!corpus)
        return;

    for(std::filesystem::directory_iterator i(corpus);
        i != std::filesystem::directory_iterator(); ++i)
    {
        if(i->path().extension() != ".pst")
            continue;

        const std::string source = i->path().string();
        std::vector<pstsdk::byte> head = slurp(widen(source));
        if(head.size() < 12)
            continue;

        pstsdk::ushort version;
        memcpy(&version, &head[10], sizeof(version));

        if(version >= pstsdk::disk::database_format_unicode_min)
        {
            strip_attachments<pstsdk::ulonglong>(source);
            drain_store<pstsdk::ulonglong>(source);
        }
        else
        {
            strip_attachments<pstsdk::ulong>(source);
            drain_store<pstsdk::ulong>(source);
        }
    }
}

template<typename T>
void check_pristine(const std::string& sample)
{
    check_refcounts<T>(pstsdk::open_database(widen(sample)), widen(sample));
}

} // end anonymous namespace

void test_delete()
{
    using std::cout;
    using std::endl;

    // the invariant the delete tests lean on has to hold before they start
    check_pristine<pstsdk::ulonglong>("test_unicode.pst");
    check_pristine<pstsdk::ulong>("test_ansi.pst");
    check_pristine<pstsdk::ulonglong>("submessage.pst");
    check_pristine<pstsdk::ulonglong>("sample1.pst");
    check_pristine<pstsdk::ulong>("sample2.pst");

    cout << "  test_block_roundtrip" << endl;
    test_block_roundtrip<pstsdk::ulonglong>("test_unicode.pst");
    test_block_roundtrip<pstsdk::ulong>("test_ansi.pst");
    test_block_roundtrip<pstsdk::ulonglong>("submessage.pst");
    test_block_roundtrip<pstsdk::ulong>("sample2.pst");

    cout << "  test_block_shrink" << endl;
    test_block_shrink<pstsdk::ulonglong>("test_unicode.pst");
    test_block_shrink<pstsdk::ulong>("test_ansi.pst");

    cout << "  test_nbt_remove" << endl;
    test_nbt_remove<pstsdk::ulonglong>("test_unicode.pst");
    test_nbt_remove<pstsdk::ulong>("test_ansi.pst");

    cout << "  test_nbt_drain" << endl;
    test_nbt_drain<pstsdk::ulonglong>("test_unicode.pst");
    test_nbt_drain<pstsdk::ulong>("test_ansi.pst");

    cout << "  test_delete_node" << endl;
    test_delete_node<pstsdk::ulonglong>("test_unicode.pst");
    test_delete_node<pstsdk::ulong>("test_ansi.pst");
    test_delete_node<pstsdk::ulonglong>("submessage.pst");

    cout << "  test_pc_set_inline" << endl;
    test_pc_set_inline<pstsdk::ulonglong>("test_unicode.pst");
    test_pc_set_inline<pstsdk::ulong>("test_ansi.pst");

    cout << "  test_tc_remove_row" << endl;
    test_tc_remove_row<pstsdk::ulonglong>("test_unicode.pst");
    test_tc_remove_row<pstsdk::ulong>("test_ansi.pst");
    test_tc_remove_row<pstsdk::ulonglong>("submessage.pst");

    cout << "  test_delete_message" << endl;
    test_delete_message<pstsdk::ulonglong>("test_unicode.pst");
    test_delete_message<pstsdk::ulong>("test_ansi.pst");
    test_delete_message<pstsdk::ulonglong>("submessage.pst");

    cout << "  test_delete_folder" << endl;
    test_delete_folder<pstsdk::ulonglong>("test_unicode.pst");
    test_delete_folder<pstsdk::ulong>("test_ansi.pst");
    test_delete_folder<pstsdk::ulonglong>("submessage.pst");

    cout << "  test_delete_attachment" << endl;
    test_delete_attachment<pstsdk::ulonglong>("submessage.pst");

    cout << "  test_wipe_free_space" << endl;
    test_wipe_free_space<pstsdk::ulonglong>(
        "submessage.pst", L"This is a message which has an embedded message attached");

    cout << "  test_corpus" << endl;
    test_corpus();
}
