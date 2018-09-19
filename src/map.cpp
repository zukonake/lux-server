#include <lux_shared/common.hpp>
#include <lux_shared/util/packer.hpp>
#include <lux_shared/map.hpp>
//
#include <db.hpp>
#include "map.hpp"

HashMap<ChkPos, Chunk, util::Packer<ChkPos>> chunks;

struct LightNode {
    ChkIdx   idx;
    Vec3<U8> col;
};
HashSet<ChkPos, util::Packer<ChkPos>> lightning_updates;
HashMap<ChkPos, Queue<LightNode>, util::Packer<ChkPos>> lightning_nodes;

static void update_lightning(ChkPos const &pos);
static bool is_chunk_loaded(ChkPos const& pos) {
    return chunks.count(pos) > 0;
}

void map_tick() {
    for(auto it = lightning_updates.begin(); it != lightning_updates.end();) {
        if(is_chunk_loaded(*it)) {
            update_lightning(*it);
            it = lightning_updates.erase(it);
        } else it++;
    }
}

static Chunk& load_chunk(ChkPos const& pos) {
    LUX_ASSERT(!is_chunk_loaded(pos));
    LUX_LOG("loading chunk");
    LUX_LOG("    pos: {%zd, %zd, %zd}", pos.x, pos.y, pos.z);
    ///@RESEARCH to do a better way to no-copy default construct
    Chunk& chunk = chunks[pos];
    for(Uns i = 0; i < CHK_VOL; ++i) {
        VoxelId voxel_id = db_voxel_id("void");
        MapPos map_pos = to_map_pos(pos, i);
        if(map_pos.x % 8 == 0 || map_pos.y % 8 == 0) {
            voxel_id = db_voxel_id("stone_wall");
        } else {
            voxel_id = db_voxel_id("stone_floor");
        }
        chunk.voxels[i] = voxel_id;
        chunk.light_lvls[i] = 0;
    }
    add_light_node(to_map_pos(pos, 34), {0xF, 0xF, 0xF});
    return chunk;
}

void guarantee_chunk(ChkPos const& pos) {
    if(!is_chunk_loaded(pos)) {
        load_chunk(pos);
    }
}

Chunk const& get_chunk(ChkPos const& pos) {
    LUX_ASSERT(is_chunk_loaded(pos));
    ///wish there was a non-bound-checking way to do it
    return chunks.at(pos);
}

void add_light_node(MapPos const& pos, Vec3<U8> col) {
    ChkPos chk_pos = to_chk_pos(pos);
    LUX_LOG("%zd, %zd", pos.x, pos.y);
    lightning_nodes[chk_pos].push({to_chk_idx(pos), col});
    lightning_updates.insert(chk_pos);
}

static void update_lightning(ChkPos const &pos)
{
    if(lightning_nodes.count(pos) == 0) return;

    Chunk& chunk = chunks.at(pos);
    auto &nodes = lightning_nodes.at(pos);
    while(!nodes.empty()) {
        LightNode node = nodes.front();
        nodes.pop();

        if(db_voxel_type(chunk.voxels[node.idx]).shape == VoxelType::BLOCK) {
            chunk.light_lvls[node.idx] = 0x0000;
            continue;
        }

        constexpr Vec3I offsets[6] =
            {{-1,  0,  0}, { 1,  0,  0},
             { 0, -1,  0}, { 0,  1,  0},
             { 0,  0, -1}, { 0,  0,  1}};

        MapPos base_pos = to_map_pos(pos, node.idx);
        //TODO bit-level parallelism

        LightLvl map_lvl = chunk.light_lvls[node.idx];
        Vec3<U8> map_color = {(map_lvl & 0xF000) >> 12,
                              (map_lvl & 0x0F00) >>  8,
                              (map_lvl & 0x00F0) >>  4};
        auto is_less = glm::lessThan(map_color + Vec3<U8>(1u), node.col);
        auto atleast_two = glm::greaterThanEqual(node.col, Vec3<U8>(2u));
        if(glm::any(is_less)) {
            /* node.col is guaranteed to be non-zero when is_less is true */
            Vec3<U8> new_color = node.col * (Vec3<U8>)is_less +
                                 map_color * (Vec3<U8>)(glm::not_(is_less));
            chunk.light_lvls[node.idx] = (new_color.r << 12) |
                                         (new_color.g <<  8) |
                                         (new_color.b <<  4);
            if(glm::any(atleast_two)) {
                Vec3<U8> side_color = node.col - (Vec3<U8>)atleast_two;
                for(auto const &offset : offsets) {
                    //@TODO don't spread lights through Z if there is floor
                    MapPos map_pos = base_pos + offset;
                    ChkPos chk_pos = to_chk_pos(map_pos);
                    ChkIdx idx     = to_chk_idx(map_pos);
                    if(chk_pos == pos) {
                        nodes.push({idx, side_color});
                    } else {
                        lightning_nodes[chk_pos].push({idx, side_color});
                        lightning_updates.insert(chk_pos);
                    }
                }
            }
        }
    }
}
