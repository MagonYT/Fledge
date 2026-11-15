// CameraFollow - smooth third-person follow camera. Orbit with the mouse (LookX),
// runs in OnLateUpdate so it sees the player's final position for the frame.
#include <fledge/script/script.h>

using namespace fledge::script;

class CameraFollow : public Behaviour {
public:
    FLEDGE_PROPERTY(Entity, target, Entity(), Tooltip("Entity to follow (the player)"));
    FLEDGE_PROPERTY(float, distance, 5.5f, Range(1, 30));
    FLEDGE_PROPERTY(float, height, 2.2f, Range(-5, 10));
    FLEDGE_PROPERTY(float, lookHeight, 1.3f, Range(0, 5), Tooltip("Aim point above the target's origin"));
    FLEDGE_PROPERTY(float, mouseSensitivity, 0.25f, Range(0, 2));
    FLEDGE_PROPERTY(float, sharpness, 10.0f, Range(1, 40), Tooltip("Follow smoothing"));

    void OnStart() override {
        if (!target) target = Scene::FindWithTag("player");
    }

    void OnLateUpdate(float dt) override {
        if (!target) return;
        m_yaw -= Input::Axis("LookX") * mouseSensitivity * 0.01f;
        Vec3 offset = Quat::FromEuler(0, m_yaw).Rotate(Vec3(0, 0, distance));
        Vec3 focus = target.Position() + Vec3(0, lookHeight, 0);
        Vec3 desired = target.Position() + Vec3(0, height, 0) + offset;
        Entity cam = GetEntity();
        cam.SetPosition(Vec3::Lerp(cam.Position(), desired, Damp(sharpness, dt)));
        cam.LookAt(focus);
    }

private:
    float m_yaw = 0;
};
FLEDGE_BEHAVIOUR(CameraFollow)
