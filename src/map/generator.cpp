#include <functional>
//
#include <lux/alias/scalar.hpp>
//
#include <data/database.hpp>
#include <data/config.hpp>
#include <map/chunk.hpp>
#include "generator.hpp"

Generator::Generator(PhysicsEngine &physics_engine, data::Config const &config) :
    config(config),
    physics_engine(physics_engine)
{

}

void Generator::generate_chunk(Chunk &chunk, chunk::Pos const &pos)
{
    num_gen.seed(std::hash<chunk::Pos>()(pos));
    chunk.tiles.reserve(chunk::TILE_SIZE);
    for(SizeT i = 0; i < chunk::TILE_SIZE; ++i)
    {
        map::Pos map_pos = chunk::to_map_pos(pos, i);
        if((map_pos.x % 8 == 0 ||
            map_pos.y % 8 == 0) &&
            map_pos.z % 4 == 0)
        {
            if(num_gen() % 6 >= 5)
            {
                chunk.tiles.emplace_back(*config.db->tile_types.at("stone_floor"));
            }
            else
            {
                chunk.tiles.emplace_back(*config.db->tile_types.at("stone_wall"));
                physics_engine.add_block(map_pos);
            }
        }
        else
        {
            chunk.tiles.emplace_back(*config.db->tile_types.at("stone_floor"));
        }
    }
}
