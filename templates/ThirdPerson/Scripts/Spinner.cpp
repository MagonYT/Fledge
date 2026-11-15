// Spinner - rotates the entity around an axis.
#include <fledge/script/script.h>

using namespace fledge::script;

class Spinner : public Behaviour {
public:
    FLEDGE_PROPERTY(float, degreesPerSecond, 90.0f, Range(-720, 720));
    FLEDGE_PROPERTY(Vec3, axis, Vec3(0, 1, 0));

    void OnUpdate(float dt) override { GetEntity().Rotate(Quat::FromAxisAngle(axis, Radians(degreesPerSecond) * dt)); }
};
FLEDGE_BEHAVIOUR(Spinner)
