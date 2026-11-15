// ThirdPersonController - input -> movement -> facing, relative to the camera.
// WASD / arrows move, LeftShift sprints, Space jumps. No physics yet: the player
// stays on its start height (simple jump arc) and passes through obstacles.
#include <fledge/script/script.h>

using namespace fledge::script;

class ThirdPersonController : public Behaviour {
public:
    FLEDGE_PROPERTY(float, walkSpeed, 4.0f, Range(0, 20), Tooltip("m/s"));
    FLEDGE_PROPERTY(float, sprintSpeed, 8.0f, Range(0, 30), Tooltip("m/s while Sprint is held"));
    FLEDGE_PROPERTY(float, turnSharpness, 12.0f, Range(0, 40), Tooltip("How fast the character turns to face its movement"));
    FLEDGE_PROPERTY(float, jumpSpeed, 5.0f, Range(0, 20), Tooltip("Initial upward speed of a jump (m/s)"));
    FLEDGE_PROPERTY(float, gravity, 15.0f, Range(0, 50), Tooltip("m/s^2"));
    FLEDGE_PROPERTY(Entity, camera, Entity(), Tooltip("Camera that orients the input; empty = the entity named 'Follow Camera'"));

    void OnStart() override {
        if (!camera) camera = Scene::Find("Follow Camera");
        m_groundY = GetEntity().Position().y;
        if (!IsHotReloading()) Log::Info("ThirdPersonController ready (camera: {})", camera ? camera.Name() : "none");
    }

    void OnUpdate(float dt) override {
        Entity self = GetEntity();
        Vec3 forward = camera ? camera.Forward() : Vec3::Forward();
        forward.y = 0;
        forward = forward.Normalized();
        Vec3 right = Vec3::Cross(forward, Vec3::Up());
        Vec3 move = right * Input::Axis("MoveX") + forward * Input::Axis("MoveY");
        if (move.LengthSquared() > 1) move = move.Normalized();
        float speed = Input::Down("Sprint") ? sprintSpeed : walkSpeed;
        Vec3 pos = self.Position() + move * speed * dt;

        bool grounded = pos.y <= m_groundY + 1e-3f;
        if (Input::Pressed("Jump") && grounded) m_verticalSpeed = jumpSpeed;
        m_verticalSpeed -= gravity * dt;
        pos.y += m_verticalSpeed * dt;
        if (pos.y < m_groundY) {
            pos.y = m_groundY;
            m_verticalSpeed = 0;
        }
        self.SetPosition(pos);
        if (move.LengthSquared() > 1e-4f)
            self.SetRotation(Quat::Slerp(self.Rotation(), Quat::LookRotation(move), Damp(turnSharpness, dt)));
    }

private:
    float m_groundY = 0, m_verticalSpeed = 0;
};
FLEDGE_BEHAVIOUR(ThirdPersonController)
