#include <ash/memory/buddy_system.h>
#include <ash/pointer.h>
#include <assert.h>
#include <string.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

namespace ash {

buddy_system::buddy_system() {
    memset(&_rgn, 0, sizeof _rgn);
    _align = 0;
    _max_blk_size = 0;
    _flist_v = nullptr;
    _total_allocated_size = 0;
    memset(&_status, 0, sizeof(buddy_impl::buddy_system_status));
}

buddy_system::~buddy_system() {
    _cleanup();
}

buddy_system::buddy_system(memrgn_t const& rgn, unsigned const align, unsigned const min_cof) :
    buddy_system() {
    init(rgn, align, min_cof);
}

void buddy_system::init(memrgn_t const& rgn, unsigned const align, unsigned const min_cof) {
    //TODO: badalloc handling
    using namespace buddy_impl;
    assert(ash::is_aligned_address(_rgn.ptr, align));
    cof_type const root_cof = static_cast<cof_type>(rgn.size / align);
    _rgn = rgn;
    _align = align;
    _max_blk_size = root_cof * align;
    _tbl.init(root_cof, align, min_cof);
    auto block = _block_pool.malloc();
    block->cof = root_cof;
    block->blkidx = 0;
    block->rgn = _rgn;
    block->pair = nullptr;
    block->parent = nullptr;
    block->in_use = false;
    _flist_v = _init_free_list_vec(_tbl.size(), _node_pool);
    _flist_v[0].emplace_front(block);
    _route.reserve(_tbl.max_level());
    _total_allocated_size = 0;
    fprintf(stdout, "Buddy system is online. [%p, %" PRIu64 "]\n", rgn.ptr, rgn.size);
}

/*
 * * Root: 200
 * Minimum coefficient: 3
 *
 * Coefficient tree
 *              +---          [232]             ---> 232 [Root]
 *              |               |
 *              |             [116]             ---> 116
 *   Linear  <--+               |
 *              |             [58]              ---> 58
 *              |               |
 *              +---          [29]              ---> 29
 *              |            /    \
 *              |         [15]     [14]         ---> 15, 14
 *   Binary  <--+        /  \       /  \
 *              |      [8]  [7]    [7]  [7]     ---> 8, 7 [A1B3-Pattern]
 *              |      / \  / \    / \   / \
 *              +--- [4][4][4][3] [4][3][4][3]  ---> 4, 3 [A3B1-Pattern]
 *
 * Buddy table
 * - Flag: Unique (U), Frequent (F), Rare (R), A1B3-Pattern, A3B1-Pattern
 * +-----+-----+-------+---+---+---+---------+------+-----+
 * | Idx | Lev |  Cof  | U | F | R | Pattern | Dist | Off |
 * +-----+-----+-------+---+---+---+---------+------+-----+
 * |  0  |  0  |  232  | # |   |   |   N/A   |   0  |  0  | => 0: U, Root
 * |  1  |  1  |  116  | # |   |   |   N/A   |   1  |  0  | => 1: U
 * |  2  |  2  |   58  | # |   |   |   N/A   |   1  |  0  | => 2: U
 * |  3  |  3  |   29  | # |   |   |   N/A   |   1  |  0  | => 3: U
 * |  4  |  4  |   15  |   |   | # |   A3B1  |   1  |  0  | => 4: R-A3B1*
 * |  5  |  4  |   14  |   |   | # |   A3B1  |   2  |  1  | => 5: R-A3B1*
 * |  6  |  5  |    8  |   |   | # |   A1B3  |   2  |  0  | => 6: R-A1B3
 * |  7  |  5  |    7  |   | # |   |   A1B3  |   3  |  1  | => 7: F-A1B3
 * |  8  |  6  |    4  |   | # |   |   A3B1  |   2  |  0  | => 8: F-A3B1
 * |  9  |  6  |    3  |   |   | # |   A3B1  |   3  |  1  | => 9: R-A3B1
 * +-----+-----+-------+---+---+---+---------+------+-----+
 * Note that the patterns of a first binary level (4) are assumed to be R-A3B1*
 *
 * Initial state of:
 * Free-list vector         | Allocation tree (* means free node)
 * +-----+-----------+      |               [232:0x00]*
 * | Idx | Free-list |      |
 * +-----+-----------+      |
 * |  0  |    0x00   |      |
 * |  1  |    NULL   |      |
 * |  2  |    NULL   |      |
 * |  3  |    NULL   |      |
 * |  4  |    NULL   |      |
 * |  5  |    NULL   |      |
 * |  6  |    NULL   |      |
 * |  7  |    NULL   |      |
 * |  8  |    NULL   |      |
 * |  9  |    NULL   |      |
 * +-----+-----------+      |
 *
 * Create a route of seed 9 for a first allocation:
 * +------+-----+--------+------------+------+--------+---------------------+
 * | Step | Idx | Lookup | Properties | Cand | Parent |        Route        |
 * +------+-----+--------+------------+------+--------+---------------------+
 * |   0  |  9  |  MISS  |   R-A3B1   | 8, 9 |  6, 7  | 8                   |
 * |  1-1 |  6  |  MISS  |   R-A1B3   |   6  |    4   |                     |
 * |  1-2 |  7  |  MISS  |   F-A1B3   | 6, 7 |  4, 5  | 8->7                |
 * |  2-1 |  5  |  MISS  |   R-A3B1*  |   5  |    3   |                     |
 * |  2-2 |  4  |  MISS  |   R-A3B1*  |   4  |    3   | 8->7->4             |
 * |   3  |  3  |  MISS  |      U     |   3  |    2   | 8->7->4->3          |
 * |   4  |  2  |  MISS  |      U     |   2  |    1   | 8->7->4->3->2       |
 * |   5  |  1  |  MISS  |      U     |   1  |    0   | 8->7->4->3->2->1    |
 * |   6  |  0  |   HIT  |      U     |   0  |   NULL | 8->7->4->3->2->1->0 |
 * +------+-----+--------+------------+------+--------+---------------------+
 *
 * States after the first allocation:
 * Free-list vector         | Allocation tree (* means free node)
 * +-----+-----------+      |                          [232:0x00]
 * | Idx | Free-list |      |                          /         \
 * +-----+-----------+      |                  [116:0x10]        [116:0x11]*
 * |  0  |    NULL   |      |                  /         \
 * |  1  |    0x11   |      |          [58:0x20]         [58:0x21]*
 * |  2  |    0x21   |      |           |       \
 * |  3  |    0x31   |      |        [29:0x30]  [29:0x31]*
 * |  4  |    0x41   |      |           |     \
 * |  5  |    NULL   |      |      [15:0x40]  [14:0x41]*
 * |  6  |    NULL   |      |        |      \
 * |  7  |    0x50   |      |    [8:0x50]*  [7:0x51]
 * |  8  |    NULL   |      |                /     \
 * |  9  |    0x61   |      |           [4:0x60]  [3:0x61]*
 * +-----+-----------+      |              ^
 *                          |              |
 *                          |              +-- return this (request: 3, result 4)
 *
 * Create a route of seed 9 for a second allocation:
 * +------+-----+--------+------------+------+--------+---------------------+
 * | Step | Idx | Lookup | Properties | Cand | Parent |        Route        |
 * +------+-----+--------+------------+------+--------+---------------------+
 * |   0  |  9  |   HIT  |   R-A3B1   | 8, 9 |  6, 7  | 9 (Cache hit)       |
 * +------+-----+--------+------------+------+--------+---------------------+
 *
 * States after the second allocation:
 * Free-list vector         | Allocation tree (* means free node)
 * +-----+-----------+      |                          [232:0x00]
 * | Idx | Free-list |      |                          /         \
 * +-----+-----------+      |                  [116:0x10]        [116:0x11]*
 * |  0  |    NULL   |      |                  /         \
 * |  1  |    0x11   |      |          [58:0x20]         [58:0x21]*
 * |  2  |    0x21   |      |           |       \
 * |  3  |    0x31   |      |        [29:0x30]  [29:0x31]*
 * |  4  |    0x41   |      |           |     \
 * |  5  |    NULL   |      |      [15:0x40]  [14:0x41]*
 * |  6  |    NULL   |      |        |      \
 * |  7  |    0x50   |      |    [8:0x50]*  [7:0x51]
 * |  8  |    NULL   |      |                /     \
 * |  9  |    NULL   |      |           [4:0x60]  [3:0x61]*
 * +-----+-----------+      |                        ^
 *                          |                        |
 *                          |                        +-- here (request: 3, result 3)
 *
 */

} // !namespace ash