#pragma once

#include <lux/alias/scalar.hpp>
#include <lux/alias/string.hpp>
#include <lux/common/entity.hpp>

struct EntityType
{
    EntityType(String const &_str_id, String const &_name) :
        str_id(_str_id), name(_name)
    {

    }

    EntityId id;
    String str_id;
    String name;
};