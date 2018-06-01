#pragma once

#include <cstddef>
//
#include <linear/size_3d.hpp>
#include <world/map/chunk/index.hpp>
#include <world/map/chunk/point.hpp>
#include <world/map/point.hpp>
#include <world/tile.hpp>

namespace world::map
{
inline namespace chunk
{

struct Chunk
{
    static constexpr linear::Size3d<std::size_t> SIZE = {16, 16, 3};
    static const std::size_t TILE_SIZE = SIZE.x * SIZE.y * SIZE.z;

    Chunk(Tile *tiles);

    static chunk::Point point_map_to_chunk(Point point);
    static chunk::Index point_map_to_index(Point point);

    Tile *tiles;
};

}
}
