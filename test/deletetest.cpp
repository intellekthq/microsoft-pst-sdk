#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

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

// Shrinking is the only resize primitive B allows, and the interesting case is a
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

        // growth has nowhere to go, so it must be refused outright
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
// exercises primitive A on its own.
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

} // end anonymous namespace

void test_delete()
{
    test_block_roundtrip<pstsdk::ulonglong>("test_unicode.pst");
    test_block_roundtrip<pstsdk::ulong>("test_ansi.pst");
    test_block_roundtrip<pstsdk::ulonglong>("submessage.pst");
    test_block_roundtrip<pstsdk::ulong>("sample2.pst");

    test_block_shrink<pstsdk::ulonglong>("test_unicode.pst");
    test_block_shrink<pstsdk::ulong>("test_ansi.pst");

    test_nbt_remove<pstsdk::ulonglong>("test_unicode.pst");
    test_nbt_remove<pstsdk::ulong>("test_ansi.pst");

    test_nbt_drain<pstsdk::ulonglong>("test_unicode.pst");
    test_nbt_drain<pstsdk::ulong>("test_ansi.pst");
}
