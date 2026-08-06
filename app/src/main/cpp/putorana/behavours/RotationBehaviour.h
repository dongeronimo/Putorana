#ifndef ARRECONSTRUCTOR_ROTATIONBEHAVIOUR_H
#define ARRECONSTRUCTOR_ROTATIONBEHAVIOUR_H

#include "BaseBehaviour.h"

#include <glm/glm.hpp>

namespace putorana::behaviours {

    //Spins the owning node at a constant rate.
    //
    //It drives the node's EULER angles rather than composing quaternions, and
    //that is the whole reason it is this short. Node keeps the angles it was
    //last handed verbatim and unclamped, so the value simply climbs past 360
    //and keeps going; composing a delta quaternion every frame would instead
    //renormalise a slightly different rotation sixty times a second and drift.
    //See the class comment in Node.h, which describes this exact case.
    //
    //Local rotation, so a node under a spinning parent inherits the parent's
    //spin on top of its own.
    class RotationBehaviour : public BaseBehaviour {
    public:
        //Degrees per second about each axis. Public on purpose: it is a plain
        //setting with nothing derived from it, so anything may change it at any
        //time and the next update simply uses the new value.
        //
        //The axes are Node's Euler convention, which is Unity's: degrees,
        //applied Z then X then Y about WORLD axes.
        glm::vec3 degreesPerSecond{0.0f, 0.0f, 0.0f};

        explicit RotationBehaviour(putorana::graphics::Node* node)
            : BaseBehaviour(node) {}

        RotationBehaviour(putorana::graphics::Node* node, const glm::vec3& degreesPerSecond)
            : BaseBehaviour(node), degreesPerSecond(degreesPerSecond) {}

        void update(float dt) override;
    };

}

#endif //ARRECONSTRUCTOR_ROTATIONBEHAVIOUR_H
