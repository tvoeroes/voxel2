#include "VoxelStorage.hpp"

#include <iostream>

#include <algorithm>
#include <iterator>
#include <cassert>
#include <cstdio>
#include <memory>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zlib.h>

// TODO: remove
#include <cmath>

//==============================================================================
constexpr VoxelStorage::u32Vec3 VoxelStorage::CHUNK_POSITION_HASH_SEED;
constexpr VoxelStorage::u32Vec3 VoxelStorage::REGION_POSITION_HASH_SEED;
constexpr VoxelStorage::s32Vec3 VoxelStorage::REGION_SIZE;
constexpr VoxelStorage::s32Vec3 VoxelStorage::CHUNK_SIZE;

//==============================================================================
template<typename T>
static bool all_equal(const T & a, const T & b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

//==============================================================================
template<typename T>
static T mul(const T & a, const T & b) {
    return T{ a.x * b.x, a.y * b.y, a.z * b.z };
}

//==============================================================================
template<typename T>
static T floor_div(const T & a, const T & b) {
    // compiler might not generate best possible code
    // in case second arument is constexpr power of 2
    T tmp;
    tmp.x = (a.x + (a.x < 0)) / b.x - (a.x < 0);
    tmp.y = (a.y + (a.y < 0)) / b.y - (a.y < 0);
    tmp.z = (a.z + (a.z < 0)) / b.z - (a.z < 0);
    return tmp;
}

//==============================================================================
template<typename T>
static T floor_mod(const T & a, const T & b) {
    // compiler might not generate best possible code
    // in case second arument is constexpr power of 2
    T tmp;
    tmp.x = (a.x % b.x + b.x) % b.x;
    tmp.y = (a.y % b.y + b.y) % b.y;
    tmp.z = (a.z % b.z + b.z) % b.z;
    return tmp;
}

//==============================================================================
template<typename T>
static typename T::element_type to_index(const T & p, const T & s) {
    return p.z * s.y * s.x + p.y * s.x + p.x;
}

//==============================================================================
template<typename T>
static typename T::element_type position_to_index(const T & p, const T & s) {
    return to_index(floor_mod(p, s), s);
}


//==============================================================================
//==============================================================================
//==============================================================================
//==============================================================================
//==============================================================================


//==============================================================================
VoxelStorage::VoxelStorage() : c_maximum_compressed_size{ compressBound(CHUNK_VOLUME * sizeof(Block)) } {
    std::cout << "Memory usage in bytes:" << std::endl;
    std::cout << "Chunk Heap: " << sizeof(Chunk) * CHUNK_HEAP_SIZE << std::endl;
    std::cout << "Region Heap: " << sizeof(Region) * REGION_HEAP_SIZE << std::endl;
    for (uint32_t i = 0; i < CHUNK_HEAP_SIZE; ++i) {
        m_chunk_LRU.add_to_heap(new ChunkNode);
    }
    for (uint32_t i = 0; i < REGION_HEAP_SIZE; ++i) {
        m_region_LRU.add_to_heap(new RegionNode);
    }
    m_compress_buffer = std::make_unique<uint8_t[]>(c_maximum_compressed_size);
    m_defragment_buffer_raw = std::make_unique<uint32_t[]>(REGION_VOLUME * 2);
    m_defragment_buffer = std::make_unique<u32Vec3[]>(REGION_VOLUME);
}

//==============================================================================
VoxelStorage::~VoxelStorage() {
    while (true) {
        ChunkNode * chunk = m_chunk_LRU.remove_lru_node();
        if (chunk == nullptr) break;
        close_chunk(chunk);
        m_chunk_LRU.add_to_heap(chunk);
    }
    while (true) {
        RegionNode * region = m_region_LRU.remove_lru_node();
        if (region == nullptr) break;
        close_region(region);
        m_region_LRU.add_to_heap(region);
    }

    while (true) {
        ChunkNode * chunk = m_chunk_LRU.get_from_heap();
        if (chunk == nullptr) break;
        delete chunk;
    }
    while (true) {
        RegionNode * region = m_region_LRU.get_from_heap();
        if (region == nullptr) break;
        delete region;
    }
}

//==============================================================================
VoxelStorage::Block * VoxelStorage::get(int32_t x, int32_t y, int32_t z, bool cache, bool edit) {
    ChunkNode * c = get_chunk({ x, y, z });
    if (edit) c->val.dirty = true;
    // TODO: if (!cache) do not put in cache (get_chunk puts it in cache)
    return c->val.blocks.data();
}

//==============================================================================
void VoxelStorage::create_new_chunk(ChunkNode * chunk, const s32Vec3 & chunk_position) {
    assert(chunk != nullptr);
#if 0
    for (Block * b = chunk->blocks, * end = chunk->blocks + CHUNK_VOLUME; b < end; ++b) {
        *b = 0;
    }
    chunk->blocks[CHUNK_VOLUME / 2] = 10;
#else
    const s32Vec3 from_block{ chunk_position.x * CHUNK_SIZE.x, chunk_position.y * CHUNK_SIZE.y, chunk_position.z * CHUNK_SIZE.z };
    const s32Vec3 to_block{ from_block.x + CHUNK_SIZE.x, from_block.y + CHUNK_SIZE.y, from_block.z + CHUNK_SIZE.z };
    s32Vec3 i;
    size_t j = 0;
    for (i.z = from_block.z; i.z < to_block.z; ++i.z)
        for (i.y = from_block.y; i.y < to_block.y; ++i.y)
            for (i.x = from_block.x; i.x < to_block.x; ++i.x) {
                if (std::sin(i.x * 0.1f) * std::sin(i.z * 0.1f) * 10.0f > static_cast<float>(i.y))
                    chunk->val.blocks[j] = 1;
                else
                    chunk->val.blocks[j] = 0;
                ++j;
            }
#endif
}

//==============================================================================
VoxelStorage::ChunkNode * VoxelStorage::get_chunk(const s32Vec3 & chunk_position) {
    const uint32_t chunk_index_value = chunk_index(chunk_position);
    ChunkNode * chunk = m_chunk_LRU.get_node(chunk_position, chunk_index_value);
    if (chunk != nullptr) return chunk;
    
    // load chunk from region if it exists
    const auto region_position = floor_div(chunk_position, REGION_SIZE);
    RegionNode * region = get_region(region_position);
    assert(region != nullptr);

    chunk = m_chunk_LRU.get_from_heap();
    if (chunk == nullptr) {
        chunk = m_chunk_LRU.remove_lru_node();
        assert(chunk != nullptr);
        close_chunk(chunk);
    }
    chunk->key.x = chunk_position.x;
    chunk->key.y = chunk_position.y;
    chunk->key.z = chunk_position.z;

    const uint32_t chunk_index = static_cast<uint32_t>(position_to_index(chunk_position, REGION_SIZE));
    assert(chunk_index < REGION_VOLUME);
    
    uint32_t ps[2]; // position, size
    region->val.read(ps, 2 * sizeof(uint32_t), (2 + 2 * chunk_index) * sizeof(uint32_t));
    if (ps[0] != 0) { // if exists
        assert(ps[1] > 0 && ps[1] <= c_maximum_compressed_size);
        region->val.read(m_compress_buffer.get(), ps[1], ps[0]);
        uLongf uncompressed_size = CHUNK_VOLUME * sizeof(Block);
        const auto compression_result = uncompress(
            chunk->val.blocks.data(), &uncompressed_size,
            m_compress_buffer.get(), ps[1]
        );
        // TODO: handle zlib and file-IO errors in a better way
        if (compression_result != Z_OK || uncompressed_size != CHUNK_VOLUME * sizeof(Block))
            throw "Failed to decompress chunk. Probably bad save file.";
        chunk->val.dirty = false;
    } else {
        // TODO: should we notify that the chunk does not exist?
        //       if not, the default chunk should not be cached if the creation is trivial
        create_new_chunk(chunk, chunk_position);
        chunk->val.dirty = true;
    }

    assert(chunk != nullptr);
    m_chunk_LRU.add_node(chunk, chunk_index_value);
    return chunk;
}

//==============================================================================
uint32_t VoxelStorage::chunk_index(const s32Vec3 & chunk_position) {
    const u32Vec3 unsigned_chunk_position{
        static_cast<uint32_t>(chunk_position.x),
        static_cast<uint32_t>(chunk_position.y),
        static_cast<uint32_t>(chunk_position.z)
    };
    const u32Vec3 hash = mul(unsigned_chunk_position, CHUNK_POSITION_HASH_SEED);
    return (hash.x ^ hash.y ^ hash.z) % CHUNK_MAP_SIZE;
}

//==============================================================================
uint32_t VoxelStorage::region_index(const s32Vec3 & region_position) {
    const u32Vec3 unsigned_region_position{
        static_cast<uint32_t>(region_position.x),
        static_cast<uint32_t>(region_position.y),
        static_cast<uint32_t>(region_position.z)
    };
    const u32Vec3 hash = mul(unsigned_region_position, REGION_POSITION_HASH_SEED);
    return (hash.x ^ hash.y ^ hash.z) % REGION_MAP_SIZE;
}

//==============================================================================
void VoxelStorage::close_chunk(ChunkNode * chunk) {
    assert(chunk != nullptr);
    if (!chunk->val.dirty) return;

    RegionNode * region = get_region(floor_div(chunk->key, REGION_SIZE));
    assert(region != nullptr);
    const uint32_t chunk_index = static_cast<uint32_t>(position_to_index(chunk->key, REGION_SIZE));
    assert(chunk_index < REGION_VOLUME);

    uLongf compressed_size = c_maximum_compressed_size;
    // TODO: deflate without zlib header (save 6 bytes per chunk)
    // TODO: try lz4 or other compression algorithms
    const auto compression_result = compress2(
        m_compress_buffer.get(), &compressed_size,
        chunk->val.blocks.data(), CHUNK_VOLUME * sizeof(Block),
        Z_BEST_COMPRESSION
    );

    if (compression_result != Z_OK) throw "Zlib compression error.";

    uint32_t ops[2]; // old position, size
    region->val.read(ops, 2 * sizeof(uint32_t), (2 + 2 * chunk_index) * sizeof(uint32_t));

    const uint32_t new_size = compressed_size;
    const int64_t size_difference = static_cast<int64_t>(ops[1]) - static_cast<int64_t>(new_size);
    if (size_difference < 0) {
        // append to file
        uint32_t nps[2]{ region->val.end, new_size }; // new position, size
        region->val.write(nps, 2 * sizeof(uint32_t), (2 + 2 * chunk_index) * sizeof(uint32_t));
        region->val.write(m_compress_buffer.get(), new_size, region->val.end);
        region->val.end += new_size;
        region->val.garbage += ops[1];
    } else {
        // insert in-place
        // TODO: replace 'uint32: size' with 'uint16: size, uint16: capacity' to be able to use in-place more often
        region->val.write(m_compress_buffer.get(), new_size, ops[0]);
        region->val.garbage = region->val.garbage + static_cast<uint32_t>(size_difference);
        region->val.write(&new_size, sizeof(uint32_t), (2 + 2 * chunk_index + 1) * sizeof(uint32_t));
    }

    if (region->val.garbage >= DEFRAGMENT_GARBAGE_THRESHOLD)
        defragment_region(region);
}

//==============================================================================
void VoxelStorage::defragment_region(RegionNode * region) {
    assert(region != nullptr);
    
    uint32_t * raw_start = m_defragment_buffer_raw.get();
    uint32_t * raw_end = raw_start + REGION_VOLUME * 2;
    region->val.read(raw_start, REGION_VOLUME * 2 * sizeof(uint32_t), 2 * sizeof(uint32_t));
    u32Vec3 * vec_start = m_defragment_buffer.get();
    u32Vec3 * vec_end = vec_start; // will be set to correct value in loop
    uint32_t index = 0;
    for (uint32_t * raw = raw_start; raw != raw_end; raw += 2, ++index) {
        if (*raw != 0) {
            // x: index, y: position, z: size
            vec_end->x = index;
            vec_end->y = *raw;
            vec_end->z = *(raw + 1);
            ++vec_end;
        }
    }

    // sort by position to prevent overwriting needed data
    // (since everything will be moved towards position 0)
    std::sort(vec_start, vec_end,
        [](const u32Vec3 & a, const u32Vec3 & b) {
            return a.y < b.y;
        }
    );

    uint8_t * buffer = m_compress_buffer.get();
    uint32_t new_end = (2 * REGION_VOLUME + 2) * (uint32_t)sizeof(uint32_t);
    for (u32Vec3 * i = vec_start; i != vec_end; ++i) {
        // move to new location
        assert(i->y < ((2 * REGION_VOLUME + 2) * (uint32_t)sizeof(uint32_t)));
        assert(i->z <= c_maximum_compressed_size);
        region->val.read(buffer, i->z, i->y);
        region->val.write(buffer, i->z, new_end);
        raw_start[2 * i->x] = new_end;
        assert(raw_start[2 * i->x + 1] == i->z);
        new_end += i->z;
    }

    region->val.garbage = 0;
    region->val.end = new_end;
    // write back header
    region->val.write(raw_start, REGION_VOLUME * 2 * sizeof(uint32_t), 2 * sizeof(uint32_t));
}

//==============================================================================
VoxelStorage::RegionNode * VoxelStorage::get_region(const s32Vec3 & region_position) {
    const uint32_t region_index_value = region_index(region_position);
    RegionNode * region = m_region_LRU.get_node(region_position, region_index_value);
    if (region != nullptr) return region;

    // load region
    region = m_region_LRU.get_from_heap();
    if (region == nullptr) {
        region = m_region_LRU.remove_lru_node();
        assert(region != nullptr);
        close_region(region);
    }

    // 'world/' + 3 * numbers + 2 * '|' + '\0'
    static constexpr size_t MAX_FILE_NAME_LENGTH{ 6 + 3 * 11 + 2 + 1 };
    char file_name[MAX_FILE_NAME_LENGTH];
    static_assert(sizeof(int32_t) == sizeof(int));
    std::snprintf(
        file_name, MAX_FILE_NAME_LENGTH, "world/%i|%i|%i",
        region_position.x, region_position.y, region_position.z
    );
    //Region * r = region->val;
    const bool is_new_file = region->val.open_file(file_name);
    if (is_new_file) {
        // initialize region file
        region->val.end = (2 * REGION_VOLUME + 2) * (uint32_t)sizeof(uint32_t);
        region->val.garbage = 0;
        ftruncate(region->val.fd, region->val.end);
    } else {
        uint32_t eg[2];
        region->val.read(eg, 2 * sizeof(uint32_t), 0);
        region->val.end = eg[0];
        region->val.garbage = eg[1];
    }
    region->key.x = region_position.x;
    region->key.y = region_position.y;
    region->key.z = region_position.z;

    m_region_LRU.add_node(region, region_index_value);
    
    assert(region != nullptr);
    return region;
}

//==============================================================================
void VoxelStorage::close_region(RegionNode * region) {
    assert(region != nullptr);
    if (region->val.fd < 0) return;
    uint32_t eg[2]{ region->val.end, region->val.garbage };
    region->val.write(eg, 2 * sizeof(uint32_t), 0);
    close(region->val.fd);
}


//==============================================================================
//==============================================================================
//==============================================================================
//==============================================================================
//==============================================================================


//==============================================================================
template<typename T, uint32_t N>
T * VoxelStorage::LeastRecentlyUsed<T, N>::get_node(const s32Vec3 & node_position, uint32_t node_index) {
    assert(node_index < N);
    T * node = m_map[node_index];
    while (node != nullptr && !all_equal(node_position, node->key))
        node = node->down;

    // move to most recently used
    if (node != nullptr) {
        // TODO: only move in map if not top
        // TODO: only move in list if not front
        remove_node(node);
        add_node(node, node_index);
    }
    return node;
}

//==============================================================================
template<typename T, uint32_t N>
void VoxelStorage::LeastRecentlyUsed<T, N>::add_node(T * node, uint32_t index) {
    assert(node != nullptr);
    // add to map
    assert(index < N);
    node->head = m_map + index;
    node->down = m_map[index];
    m_map[index] = node;
    // add to list
    node->prev = nullptr;
    node->next = m_front;
    if (m_front != nullptr)
        m_front->prev = node;
    else
        m_back = node;
    m_front = node;
}

//==============================================================================
template<typename T, uint32_t N>
T * VoxelStorage::LeastRecentlyUsed<T, N>::remove_lru_node() {
    T * lru = m_back;
    if (lru != nullptr)
        remove_node(lru);
    return lru;
}

//==============================================================================
template<typename T, uint32_t N>
void VoxelStorage::LeastRecentlyUsed<T, N>::remove_node(T * node) {
    assert(node != nullptr);

    // remove from map
    T * * i = node->head;
    while (*i != node) {
        assert(*i != nullptr);
        i = &(*i)->down;
    }
    *i = node->down;

    // remove from list
    if (node->prev != nullptr)
        node->prev->next = node->next;
    else
        m_front = node->next;
    if (node->next != nullptr)
        node->next->prev = node->prev;
    else
        m_back = node->prev;
}

//==============================================================================
template<typename T, uint32_t N>
VoxelStorage::LeastRecentlyUsed<T, N>::LeastRecentlyUsed() {
    std::fill(std::begin(m_map), std::end(m_map), nullptr);
    m_free_list = nullptr;
    m_front = nullptr;
    m_back = nullptr;
}

//==============================================================================
template<typename T, uint32_t N>
void VoxelStorage::LeastRecentlyUsed<T, N>::add_to_heap(T * node) {
    assert(node != nullptr);
    node->next = m_free_list;
    m_free_list = node;
}

//==============================================================================
template<typename T, uint32_t N>
T * VoxelStorage::LeastRecentlyUsed<T, N>::get_from_heap() {
    T * node = m_free_list;
    if (node != nullptr)
        m_free_list = node->next;
    return node;
}


//==============================================================================
//==============================================================================
//==============================================================================
//==============================================================================
//==============================================================================


//==============================================================================
void VoxelStorage::Region::read(void * buffer, size_t count, size_t position) {
    pread(fd, buffer, count, position);
}

//==============================================================================
void VoxelStorage::Region::write(const void * buffer, size_t count, size_t position) {
    pwrite(fd, buffer, count, position);
}

//==============================================================================
bool VoxelStorage::Region::open_file(const char * const file_name) {
    fd = open(file_name, O_RDWR | O_CREAT, 0666);
    struct stat file_info;
    const auto result = fstat(fd, &file_info);
    return file_info.st_size == 0;
}
