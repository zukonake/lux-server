#include <entity/type.hpp>
#include <map/tile_type.hpp>
#include <data/database.hpp>
#include <data/config.hpp>

const entity::Type human =
{
    "human",
    "Human"
};

const map::TileType void_tile =
{
    "void",
    "Void",
    map::TileType::EMPTY
};

const map::TileType stone_floor =
{
    "stone_floor",
    "Stone Floor",
    map::TileType::FLOOR
};

const map::TileType stone_wall =
{
    "stone_wall",
    "Stone Wall",
    map::TileType::WALL
};

const data::Database default_db =
{
    {
        {human.id, &human}
    },
    {
        {void_tile.id,  &void_tile},
        {stone_floor.id, &stone_floor},
        {stone_wall.id, &stone_wall}
    },
};

const data::Config default_config =
{
    &default_db,
    default_db.entity_types.at("human"),
    "lux server",
    64.0
};
