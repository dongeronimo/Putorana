#include "BaseBehaviour.h"
namespace putorana::behaviours {
    void BaseBehaviour::start() {
        _alreadyStarted = true;
    }

    BaseBehaviour::BaseBehaviour(putorana::graphics::Node *node) :
    owner(node) {

    }

    void BaseBehaviour::runLateUpdate(float dt) {
        //due to the way that Node works if we got here we must have a late update function defined
        //and crashing is an appropriate response to not having it here.
        (*lateUpdate)(dt, this);
    }
}