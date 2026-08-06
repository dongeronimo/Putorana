#include "RotationBehaviour.h"

#include "graphics/Node.h"

namespace putorana::behaviours {

    void RotationBehaviour::update(float dt) {
        //Read, add, write. eulerAngles() gives back exactly what was last set,
        //so this accumulates instead of resetting, and it is what keeps the
        //angle continuous through 360 rather than jumping when it wraps.
        //
        //World::Visit runs this BEFORE the node's matrix is closed, so the spin
        //lands in the same frame it was computed for.
        owner->SetEulerAngles(owner->eulerAngles() + degreesPerSecond * dt);
    }

}
