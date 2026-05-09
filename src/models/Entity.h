#pragma once

#include <string>

enum class EntityType {
    Individual,
    LLC,
    Corporation,
    Partnership,
    Trust,
    Other
};

struct Entity {
    std::string id;
    std::string name;
    EntityType type;
    std::string tax_id;
    bool is_active = true;
    std::string created_at;

    std::string type_to_string() const;
    static EntityType type_from_string(const std::string& s);
};

inline std::string Entity::type_to_string() const {
    switch (type) {
        case EntityType::Individual: return "individual";
        case EntityType::LLC: return "llc";
        case EntityType::Corporation: return "corporation";
        case EntityType::Partnership: return "partnership";
        case EntityType::Trust: return "trust";
        default: return "other";
    }
}

inline EntityType Entity::type_from_string(const std::string& s) {
    if (s == "individual") return EntityType::Individual;
    if (s == "llc") return EntityType::LLC;
    if (s == "corporation") return EntityType::Corporation;
    if (s == "partnership") return EntityType::Partnership;
    if (s == "trust") return EntityType::Trust;
    return EntityType::Other;
}