#include "fledge/gameplay/rpg.h"

#include <algorithm>

namespace fledge::gameplay {

float WantedState::heatFor(Crime c) {
    switch (c) {
        case Crime::Assault: return 8;
        case Crime::HitPedestrian: return 12;
        case Crime::VehicleTheft: return 10;
        case Crime::Shooting: return 15;
        case Crime::KillCivilian: return 25;
        case Crime::KillPolice: return 40;
        default: return 10;
    }
}

int WantedState::levelForHeat(float h) {
    if (h <= 0.5f) return 0;
    if (h < 20) return 1;
    if (h < 45) return 2;
    if (h < 80) return 3;
    if (h < 130) return 4;
    return 5;
}

void WantedState::commit(Crime c, float amount) {
    heat = std::min(200.0f, heat + (amount >= 0 ? amount : heatFor(c)));
    level = std::max(level, levelForHeat(heat));
    unseenFor = 0;
}

void WantedState::setLevel(int stars) {
    stars = std::clamp(stars, 0, 5);
    static const float kHeat[6] = {0, 10, 30, 60, 100, 150};
    heat = kHeat[stars];
    level = stars;
    unseenFor = 0;
}

void WantedState::update(float dt, bool seen) {
    if (level == 0 && heat <= 0) return;
    if (seen) {
        unseenFor = 0;
        return;
    }
    unseenFor += dt;
    if (unseenFor >= cooldown()) {  // evaded: lose one star at a time
        --level;
        static const float kHeat[6] = {0, 10, 30, 60, 100, 150};
        heat = level > 0 ? kHeat[level] : 0;
        unseenFor = 0;
    }
}

void Factions::set(const std::string& a, const std::string& b, Relation r) {
    m_rel[{a, b}] = r;
    m_rel[{b, a}] = r;
}
Relation Factions::get(const std::string& a, const std::string& b) const {
    if (a == b) return Relation::Friendly;
    auto it = m_rel.find({a, b});
    return it == m_rel.end() ? Relation::Neutral : it->second;
}
Json Factions::toJson() const {
    Json j = Json::object();
    for (auto& [k, v] : m_rel)
        if (k.first < k.second) j[k.first + "|" + k.second] = v == Relation::Hostile ? "Hostile" : v == Relation::Friendly ? "Friendly" : "Neutral";
    return j;
}
void Factions::fromJson(const Json& j) {
    if (!j.is_object()) return;
    for (auto& [k, v] : j.items()) {
        auto bar = k.find('|');
        if (bar == std::string::npos || !v.is_string()) continue;
        std::string r = v.get<std::string>();
        set(k.substr(0, bar), k.substr(bar + 1), r == "Hostile" ? Relation::Hostile : r == "Friendly" ? Relation::Friendly : Relation::Neutral);
    }
}
Factions Factions::defaults() {
    Factions f;
    f.set("player", "gang", Relation::Hostile);
    f.set("police", "gang", Relation::Hostile);
    f.set("police", "civilian", Relation::Friendly);
    return f;
}

const std::vector<WeaponDef>& weaponDefs() {
    static const std::vector<WeaponDef> w = {
        {"pistol", 25, 3, 60, 0.015f, 12, 1, 1.2f},
        {"rifle", 30, 9, 120, 0.02f, 30, 1, 2.0f},
        {"shotgun", 14, 1.2f, 30, 0.09f, 6, 8, 2.5f},
    };
    return w;
}
const WeaponDef* weaponDef(const std::string& name) {
    for (auto& w : weaponDefs())
        if (w.name == name) return &w;
    return nullptr;
}

int Inventory::count(const std::string& item) const {
    auto it = items.find(item);
    return it == items.end() ? 0 : it->second;
}
void Inventory::add(const std::string& item, int n) {
    if (item.empty() || n == 0) return;
    items[item] += n;
    if (items[item] <= 0) items.erase(item);
}
bool Inventory::remove(const std::string& item, int n) {
    if (count(item) < n) return false;
    add(item, -n);
    return true;
}
void Inventory::giveWeapon(const std::string& w, int rounds) {
    ammo[w] += rounds;
    if (current.empty()) current = w;
}
bool Inventory::tryFire(float now) {
    const WeaponDef* d = weaponDef(current);
    if (!d) return false;
    auto it = ammo.find(current);
    if (it == ammo.end() || it->second <= 0) return false;
    if (now - lastShot < 1.0f / d->fireRate) return false;
    lastShot = now;
    --it->second;
    return true;
}

} // namespace fledge::gameplay
