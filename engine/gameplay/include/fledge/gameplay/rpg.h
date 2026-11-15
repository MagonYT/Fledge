#pragma once
// Wanted level, factions, inventory and weapons-lite (docs/GAMEPLAY.md). Plain logic, no scene access.
#include <map>
#include <string>
#include <vector>

#include "fledge/core/json.h"

namespace fledge::gameplay {

// Wanted: crimes add heat. Heat maps to 0..5 stars. While police can see the player the stars hold; once the
// player has been unseen for `cooldown` seconds (10 s, plus 5 s per star) the heat drains and the stars drop.
enum class Crime { Assault = 0, HitPedestrian = 1, VehicleTheft = 2, Shooting = 3, KillCivilian = 4, KillPolice = 5, Custom = 6 };
struct WantedState {
    float heat = 0;  // 0..100+
    int level = 0;   // stars 0..5
    float unseenFor = 0;
    void commit(Crime c, float amount = -1);  // default heat per crime type
    void setLevel(int stars);                 // missions / scripts
    void update(float dt, bool seenByPolice);
    static float heatFor(Crime c);
    static int levelForHeat(float heat);
    float cooldown() const { return 10.0f + 5.0f * static_cast<float>(level); }
};

// Factions: named groups with a relation matrix. Unknown pairs are Neutral; a faction is Friendly to itself.
enum class Relation { Hostile = -1, Neutral = 0, Friendly = 1 };
class Factions {
public:
    void set(const std::string& a, const std::string& b, Relation r);  // symmetric
    Relation get(const std::string& a, const std::string& b) const;
    Json toJson() const;
    void fromJson(const Json& j);  // {"police|gang": "Hostile", ...}
    static Factions defaults();    // player, civilian, police, gang
private:
    std::map<std::pair<std::string, std::string>, Relation> m_rel;
};

struct WeaponDef {
    std::string name;
    float damage = 20, fireRate = 4;  // shots per second
    float range = 80, spread = 0.01f; // radians
    int magazine = 12, pellets = 1;
    float reload = 1.5f;
};
const std::vector<WeaponDef>& weaponDefs();  // pistol, rifle, shotgun
const WeaponDef* weaponDef(const std::string& name);

// Inventory: money, items, weapons with ammo. Serialised as the Inventory component's JSON fields.
struct Inventory {
    int money = 0;
    std::map<std::string, int> items;
    std::map<std::string, int> ammo;  // weapon -> rounds (reserve, magazine included)
    std::string current;              // equipped weapon ("" = unarmed)
    int count(const std::string& item) const;
    void add(const std::string& item, int n);
    bool remove(const std::string& item, int n);
    bool hasWeapon(const std::string& w) const { return ammo.count(w) > 0; }
    void giveWeapon(const std::string& w, int rounds);
    // Fire control: returns true when a shot leaves the barrel (ammo and cooldown permitting).
    bool tryFire(float now);
    float lastShot = -1e9f;
};

} // namespace fledge::gameplay
