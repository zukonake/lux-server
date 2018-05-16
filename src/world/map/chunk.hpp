#pragma once

#include <cstddef>
//
#include <world/map/chunk/point.hpp>
#include <world/map/point.hpp>
#include <world/tile.hpp>

namespace world::map
{

struct Chunk
{
    typedef std::size_t Index;

    static const std::size_t SIZE_X = 16; //TODO replace with a 3d vec?
    static const std::size_t SIZE_Y = 16; //
    static const std::size_t SIZE_Z = 3; //
    static const std::size_t TILE_SIZE = SIZE_X * SIZE_Y * SIZE_Z;

    static chunk::Point point_map_to_chunk(Point point);
    static Index        point_map_to_index(Point point);

    Tile tiles[TILE_SIZE];
};

}
